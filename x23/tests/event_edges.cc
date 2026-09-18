#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <ranges>
#include <semaphore>
#include <string>
#include <string_view>

#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <gtest/gtest.h>

#include "x23/event.h"
#include "x23/signal.h"
#include "x23/sockio.h"

using namespace x23;
using namespace std::chrono;

namespace {

class ManualPoll : public CoroPoll {
public:
  explicit ManualPoll(ThreadPool &pool) : CoroPoll(pool, no_loop_thread{}) {}
  using CoroPoll::check;
  using CoroPoll::request_stop;
  using CoroPoll::time_to_stop;
  using CoroPoll::wake;
};

struct SocketPair {
  UniqueFd a, b;
  SocketPair()
  {
    int fds[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds))
      Errno{}.raise("socketpair");
    a.reset(fds[0]);
    b.reset(fds[1]);
    set_nonblock(a);
    set_nonblock(b);
  }
};

} // namespace

TEST(EventEdges, GenerationChangesOnlyOnReset)
{
  ThreadPool pool(8);
  SocketPair sockets;
  ManualPoll cp(pool);
  EXPECT_EQ(cp.generation(sockets.a), 1U);
  EXPECT_EQ(cp.generation(sockets.b), 1U);
  cp.reset(sockets.b);
  EXPECT_EQ(cp.generation(sockets.b), 2U);
  EXPECT_EQ(cp.generation(sockets.a), 1U);

  for (bool conditional : {false, true}) {
    auto generation = cp.generation(sockets.a);
    auto pending = cp.read_wait(cp.read_clear(sockets.a));
    cp.write_clear(sockets.a);
    EXPECT_EQ(cp.generation(sockets.a), generation);
    if (conditional)
      cp.poison(sockets.a, generation);
    else
      cp.poison(sockets.a);
    EXPECT_EQ(pending(), EBADF);
    EXPECT_EQ(cp.generation(sockets.a), generation);
    cp.poison(sockets.a, generation);
    EXPECT_EQ(cp.read_wait(cp.read_clear(sockets.a))(), EBADF);
    EXPECT_EQ(cp.generation(sockets.a), generation);
    cp.reset(sockets.a);
    EXPECT_EQ(cp.generation(sockets.a), generation + 1);
  }

  cp.reset(sockets.a);
  EXPECT_EQ(cp.generation(sockets.a), 4U);
}

TEST(EventEdges, ConditionalPoisonBeforeActivation)
{
  ThreadPool pool(8);
  SocketPair sockets;
  ManualPoll cp(pool);
  for (unsigned i = 0; i < 2; ++i) {
    auto generation = cp.generation(sockets.a);
    cp.poison(sockets.a, generation);
    auto pending = cp.read_wait(cp.read_clear(sockets.a));
    EXPECT_TRUE(pending.operator co_await().await_ready());
    EXPECT_EQ(cp.generation(sockets.a), generation);
    cp.reset(sockets.a);
    EXPECT_EQ(pending(), EBADF);
  }
}

TEST(EventEdges, ResetInvalidatesGenerationBeforeActivation)
{
  ThreadPool pool(8);
  SocketPair sockets;
  ManualPoll cp(pool);
  auto generation = cp.generation(sockets.a);
  cp.reset(sockets.a);
  cp.poison(sockets.a, generation);
  auto pending = cp.read_wait(cp.read_clear(sockets.a));
  EXPECT_FALSE(pending.operator co_await().await_ready());
  ASSERT_EQ(::write(sockets.b.get(), "x", 1), 1);
  cp.check(0);
  EXPECT_EQ(pending(), 0);
  EXPECT_EQ(cp.generation(sockets.a), generation + 1);
}

TEST(EventEdges, GenerationDoesNotPreventShutdown)
{
  ThreadPool pool(8);
  SocketPair sockets;
  ManualPoll cp(pool);
  auto generation = cp.generation(sockets.a);
  cp.reset(sockets.a);
  cp.request_stop();
  cp.poison(sockets.a, generation);
  EXPECT_EQ(cp.generation(sockets.a), generation + 1);
  EXPECT_TRUE(cp.time_to_stop());
}

TEST(EventEdges, ReadBufferHandlesZeroLengthReadAheadAndErrors)
{
  SocketPair sockets;
  detail::GenReadBuffer<64> reader;
  std::array<char, 8> out{};
  EXPECT_EQ(reader(-1, out.data(), 0), 0);
  ASSERT_EQ(::write(sockets.b.get(), "abcdef", 6), 6);
  EXPECT_EQ(reader(sockets.a.get(), out.data(), 2), 2);
  EXPECT_EQ(std::string_view(out.data(), 2), "ab");
  EXPECT_EQ(reader.nbuffered(), 4UZ);
  EXPECT_EQ(reader(sockets.a.get(), out.data(), 3), 3);
  EXPECT_EQ(std::string_view(out.data(), 3), "cde");
  EXPECT_EQ(reader(sockets.a.get(), out.data(), 3), 1);
  EXPECT_EQ(out[0], 'f');
  EXPECT_EQ(reader.nbuffered(), 0UZ);
  EXPECT_EQ(reader(sockets.a.get(), out.data(), 1), -1);
  EXPECT_EQ(errno, EAGAIN);
  EXPECT_EQ(reader(-1, out.data(), 1), -1);
  EXPECT_EQ(errno, EBADF);
  sockets.b.reset();
  EXPECT_EQ(reader(sockets.a.get(), out.data(), 1), 0);
}

TEST(EventEdges, ReadPoisonBeforeActivationAllowsWrites)
{
  for (bool conditional : {false, true}) {
    ThreadPool pool(8);
    SocketPair sockets;
    ManualPoll cp(pool);
    auto generation = cp.generation(sockets.a);
    if (conditional)
      cp.read_poison(sockets.a, generation);
    else
      cp.read_poison(sockets.a);
    EXPECT_EQ(cp.generation(sockets.a), generation);
    EXPECT_EQ(cp.read_wait(cp.read_clear(sockets.a))(), EBADF);
    auto writing = cp.write_wait(cp.write_clear(sockets.a));
    cp.check(0);
    EXPECT_EQ(writing(), 0);
    EXPECT_EQ(cp.read_wait(cp.read_clear(sockets.a))(), EBADF);
  }
}

TEST(EventEdges, ReadPoisonFailsPendingReadsAndPreservesPendingWrites)
{
  ThreadPool pool(8);
  SocketPair sockets;
  ManualPoll cp(pool);
  auto reading = cp.read_wait(cp.read_clear(sockets.a));
  auto writing = cp.write_wait(cp.write_clear(sockets.a));
  auto generation = cp.generation(sockets.a);
  cp.read_poison(sockets.a);
  EXPECT_EQ(reading(), EBADF);
  EXPECT_FALSE(writing.operator co_await().await_ready());
  cp.read_poison(sockets.a, generation);
  EXPECT_EQ(cp.generation(sockets.a), generation);
  ASSERT_EQ(::write(sockets.b.get(), "x", 1), 1);
  cp.check(0);
  EXPECT_EQ(writing(), 0);
  EXPECT_EQ(cp.read_wait(cp.read_clear(sockets.a))(), EBADF);

  auto pending = cp.write_wait(cp.write_clear(sockets.a));
  cp.poison(sockets.a);
  EXPECT_EQ(pending(), EBADF);
  cp.read_poison(sockets.a);
  cp.read_poison(sockets.a, generation);
  EXPECT_EQ(cp.read_wait(cp.read_clear(sockets.a))(), EBADF);
  EXPECT_EQ(cp.write_wait(cp.write_clear(sockets.a))(), EBADF);
  EXPECT_EQ(cp.generation(sockets.a), generation);

  cp.reset(sockets.a);
  EXPECT_EQ(cp.generation(sockets.a), generation + 1);
  cp.read_poison(sockets.a, generation);
  auto fresh = cp.read_wait(cp.read_clear(sockets.a));
  EXPECT_FALSE(fresh.operator co_await().await_ready());
  ASSERT_EQ(::write(sockets.b.get(), "x", 1), 1);
  cp.check(0);
  EXPECT_EQ(fresh(), 0);
}

TEST(EventEdges, ReadPoisonRejectsBufferedReadsUntilReset)
{
  ThreadPool pool(8);
  SocketPair sockets;
  ManualPoll cp(pool);
  ASSERT_EQ(::write(sockets.b.get(), "xy", 2), 2);
  Buf out;
  ASSERT_EQ(cp.read(sockets.a, &out, 1)().value(), 1UZ);
  cp.read_poison(sockets.a);
  for (bool sync : {false, true}) {
    auto res = cp.read(sockets.a, &out, 1, true, sync)();
    ASSERT_FALSE(res);
    EXPECT_EQ(res.error(), EBADF);
  }
  EXPECT_EQ(out, (Buf{'x'}));
  EXPECT_EQ(cp.dwrite(sockets.a, Buf{'a'}), 0);
  EXPECT_EQ(cp.awrite(sockets.a, Buf{'b'}), 0);
  ASSERT_EQ(cp.write(sockets.a, Buf{'c'})().value(), 1UZ);
  std::array<char, 3> received{};
  ASSERT_EQ(::read(sockets.b.get(), received.data(), received.size()), 3);
  EXPECT_EQ(std::string_view(received.data(), received.size()), "abc");

  cp.reset(sockets.a);
  ASSERT_EQ(::write(sockets.b.get(), "z", 1), 1);
  ASSERT_EQ(cp.read(sockets.a, &out, 1)().value(), 1UZ);
  EXPECT_EQ(out, (Buf{'x', 'z'}));
}

TEST(EventEdges, ReadPoisonDoesNotPreventShutdown)
{
  ThreadPool pool(8);
  SocketPair sockets;
  ManualPoll cp(pool);
  auto generation = cp.generation(sockets.a);
  cp.reset(sockets.a);
  cp.request_stop();
  cp.read_poison(sockets.a, generation);
  cp.read_poison(sockets.a);
  EXPECT_EQ(cp.generation(sockets.a), generation + 1);
  EXPECT_TRUE(cp.time_to_stop());
}

TEST(EventEdges, PoisonedReadAndWritesFailUntilReset)
{
  ThreadPool pool(8);
  SocketPair sockets;
  CoroPoll cp(pool);
  cp.poison(sockets.a);
  Buf out;
  for (bool sync : {false, true}) {
    auto res = cp.read(sockets.a, &out, 1, true, sync)();
    ASSERT_FALSE(res);
    EXPECT_EQ(res.error(), EBADF);
  }
  EXPECT_EQ(cp.awrite(sockets.a, Buf{1}), EBADF);
  EXPECT_EQ(cp.dwrite(sockets.a, Buf{1}), EBADF);
  cp.reset(sockets.a);
  ASSERT_EQ(::write(sockets.b.get(), "abc", 3), 3);
  auto res = cp.read(sockets.a, &out, 8, false)();
  ASSERT_TRUE(res);
  EXPECT_EQ(*res, 3UZ);
  EXPECT_EQ(BufView(out).string_view(), "abc");
  EXPECT_EQ(cp.read(sockets.a, &out, 0)().value(), 0UZ);
}

TEST(EventEdges, DelayedWriteFlushesWhenReadIsWaiting)
{
  ThreadPool pool(8);
  SocketPair sockets;
  ManualPoll cp(pool);
  Buf out;
  auto pending = cp.read(sockets.a, &out, 1, true, true);
  ASSERT_FALSE(pending.operator co_await().await_ready());
  EXPECT_EQ(cp.dwrite(sockets.a, Buf{'x'}), 0);
  char byte{};
  ASSERT_EQ(::read(sockets.b.get(), &byte, 1), 1);
  EXPECT_EQ(byte, 'x');
  ASSERT_EQ(::write(sockets.b.get(), "y", 1), 1);
  cp.check(0);
  auto res = pending();
  ASSERT_TRUE(res);
  EXPECT_EQ(*res, 1UZ);
  EXPECT_EQ(out, (Buf{'y'}));
}

TEST(EventEdges, CloseTakesOwnershipAndAbortsPendingRead)
{
  ThreadPool pool(8);
  SocketPair sockets;
  ManualPoll cp(pool);
  cp.close(UniqueFd{});
  int fd = sockets.a.get();
  auto pending = cp.read_wait(cp.read_clear(fd));
  ASSERT_FALSE(pending.operator co_await().await_ready());
  cp.close(std::move(sockets.a));
  EXPECT_FALSE(sockets.a);
  EXPECT_EQ(pending(), EBADF);
  EXPECT_EQ(fcntl(fd, F_GETFD), -1);
  EXPECT_EQ(errno, EBADF);
  char byte;
  EXPECT_EQ(::read(sockets.b.get(), &byte, 1), 0);
}

TEST(EventEdges, CloseFlushesDelayedWritesAndResetsDescriptorState)
{
  ThreadPool pool(8);
  SocketPair sockets;
  CoroPoll cp(pool);
  int fd = sockets.a.get();
  EXPECT_EQ(cp.dwrite(fd, Buf{'a', 'b', 'c'}), 0);
  char byte;
  ASSERT_EQ(::read(sockets.b.get(), &byte, 1), -1);
  EXPECT_EQ(errno, EAGAIN);
  cp.close(std::move(sockets.a));
  Buf received;
  ASSERT_EQ(cp.read(sockets.b, &received, 4)().value(), 3UZ);
  EXPECT_EQ(received, (Buf{'a', 'b', 'c'}));
  EXPECT_EQ(fcntl(fd, F_GETFD), -1);
  EXPECT_EQ(errno, EBADF);

  SocketPair reused;
  if (reused.a.get() != fd) {
    ASSERT_EQ(dup2(reused.a.get(), fd), fd);
    reused.a.reset(fd);
  }
  Defer reset([&] { cp.reset(reused.a); });
  EXPECT_EQ(cp.awrite(reused.a, Buf{'z'}), 0);
  ASSERT_EQ(::read(reused.b.get(), &byte, 1), 1);
  EXPECT_EQ(byte, 'z');
}

TEST(EventEdges, CloseDrainsBackgroundAndDelayedWritesBeforeEof)
{
  for (bool sync : {false, true}) {
    ThreadPool pool(8);
    SocketPair sockets;
    Buf out;
    CoroPoll cp(pool);
    int size = 4096;
    ASSERT_EQ(setsockopt(sockets.a.get(), SOL_SOCKET, SO_SNDBUF, &size,
                         sizeof(size)),
              0);
    auto pending = cp.read(sockets.a, &out, 1, true, sync);
    ASSERT_FALSE(pending.operator co_await().await_ready());
    Buf data(1024 * 1024, 'a');
    EXPECT_EQ(cp.awrite(sockets.a, Buf(data)), 0);
    EXPECT_EQ(cp.dwrite(sockets.a, Buf{'b', 'c'}), 0);
    int fd = sockets.a.get();
    cp.close(std::move(sockets.a));
    EXPECT_FALSE(sockets.a);
    // The peer has not drained the socket, so close must return while
    // the descriptor is still open and the writer is suspended.
    EXPECT_NE(fcntl(fd, F_GETFD), -1);
    EXPECT_EQ(pending().error(), EBADF);
    Buf received;
    data.push_back('b');
    data.push_back('c');
    ASSERT_EQ(cp.read(sockets.b, &received, data.size() + 1)().value(),
              data.size());
    EXPECT_EQ(received, data);
    EXPECT_EQ(fcntl(fd, F_GETFD), -1);
    EXPECT_EQ(errno, EBADF);
  }
}

TEST(EventEdges, CloseReleasesDescriptorOnWriteError)
{
  ThreadPool pool(8);
  SocketPair sockets;
  CoroPoll cp(pool);
  int fd = sockets.a.get();
  EXPECT_EQ(cp.dwrite(sockets.a, Buf{'a'}), 0);
  sockets.b.reset();
  cp.close(std::move(sockets.a));
  EXPECT_EQ(fcntl(fd, F_GETFD), -1);
  EXPECT_EQ(errno, EBADF);
}

TEST(EventEdges, ClosePendingWriteIsReleasedDuringShutdown)
{
  ThreadPool pool(8);
  SocketPair sockets;
  int fd = sockets.a.get();
  {
    CoroPoll cp(pool);
    int size = 4096;
    ASSERT_EQ(setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &size, sizeof(size)), 0);
    EXPECT_EQ(cp.awrite(fd, Buf(1024 * 1024, 'a')), 0);
    cp.close(std::move(sockets.a));
    EXPECT_NE(fcntl(fd, F_GETFD), -1);
  }
  EXPECT_EQ(fcntl(fd, F_GETFD), -1);
  EXPECT_EQ(errno, EBADF);
}

TEST(EventEdges, ClosePendingWriteIsReleasedOnPeerFailure)
{
  ThreadPool pool(8);
  SocketPair sockets;
  ManualPoll cp(pool);
  int fd = sockets.a.get();
  int size = 4096;
  ASSERT_EQ(setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &size, sizeof(size)), 0);
  EXPECT_EQ(cp.awrite(fd, Buf(1024 * 1024, 'a')), 0);
  cp.close(std::move(sockets.a));
  EXPECT_NE(fcntl(fd, F_GETFD), -1);
  sockets.b.reset();
  cp.check(0);
  pool.drain();
  EXPECT_EQ(fcntl(fd, F_GETFD), -1);
  EXPECT_EQ(errno, EBADF);
}

TEST(EventEdges, WriteBackpressureCanBeAborted)
{
  ThreadPool pool(8);
  SocketPair sockets;
  ManualPoll cp(pool);
  int size = 4096;
  ASSERT_EQ(setsockopt(sockets.a.get(), SOL_SOCKET, SO_SNDBUF, &size,
                       sizeof(size)),
            0);
  Buf data(1024 * 1024, 0xa5);
  auto pending = cp.write(sockets.a, data);
  ASSERT_FALSE(pending.operator co_await().await_ready());
  cp.abort(sockets.a, true, ECANCELED);
  auto res = pending();
  ASSERT_FALSE(res);
  EXPECT_EQ(res.error(), ECANCELED);
  cp.reset(sockets.a);
  sockets.b.reset();
  res = cp.write(sockets.a, BufView(data.data(), 1))();
  ASSERT_FALSE(res);
  EXPECT_EQ(res.error(), EPIPE);
}

TEST(EventEdges, AcceptAndReadWaitsPropagateAbort)
{
  ThreadPool pool(8);
  SocketPair sockets;
  ManualPoll cp(pool);
  auto address = HostPort{"127.0.0.1", uint16_t{0}}.to_addr(false);
  ASSERT_TRUE(address);
  auto listener = stcplisten(*address);
  set_nonblock(listener);
  Defer reset([&] { cp.reset(listener); });
  auto accepting = cp.accept(listener);
  ASSERT_FALSE(accepting.operator co_await().await_ready());
  cp.abort(listener, false, ECANCELED);
  EXPECT_EQ(accepting().error(), ECANCELED);
  Buf out;
  auto reading = cp.read(sockets.a, &out, 1);
  ASSERT_FALSE(reading.operator co_await().await_ready());
  cp.abort(sockets.a, false, ETIMEDOUT);
  EXPECT_EQ(reading().error(), ETIMEDOUT);
}

TEST(EventEdges, StopRejectsNewOperations)
{
  ThreadPool pool(8);
  SocketPair sockets;
  ManualPoll cp(pool);
  cp.request_stop();
  Buf out;
  sockaddr_storage address{};
  address.ss_family = AF_INET;
  EXPECT_EQ(cp.read(sockets.a, &out, 1)().error(), EBADF);
  EXPECT_EQ(cp.connect(sockets.a, &address)().error(), EBADF);
  EXPECT_EQ(cp.tcpconnect(&address)().error(), EBADF);
  EXPECT_EQ(cp.tcpconnect("127.0.0.1", "1")().error().code().value(), EBADF);
  EXPECT_EQ(cp.accept(sockets.a)().error(), EBADF);
  EXPECT_TRUE(cp.time_to_stop());
  cp.wake();
  cp.check(0);
}

TEST(EventEdges, TcpConnectSuccessAndFailures)
{
  ThreadPool pool(8);
  CoroPoll cp(pool);
  auto address = HostPort{"127.0.0.1", uint16_t{0}}.to_addr(false);
  ASSERT_TRUE(address);
  auto listener = stcplisten(*address);
  set_nonblock(listener);
  auto bound = local_addr(listener);
  Defer reset([&] { cp.reset(listener); });
  auto connection = cp.tcpconnect(&bound)();
  ASSERT_TRUE(connection);
  UniqueFd client(*connection);
  Defer reset_client([&] { cp.reset(client); });
  sockaddr_storage peer{};
  auto accepted = cp.accept(listener, &peer, nullptr, false)();
  ASSERT_TRUE(accepted);
  UniqueFd server(*accepted);
  EXPECT_EQ(fcntl(server.get(), F_GETFD) & FD_CLOEXEC, 0);
  EXPECT_NE(fcntl(server.get(), F_GETFL) & O_NONBLOCK, 0);
  cp.close(std::move(server));

  auto invalid_service = cp.tcpconnect("127.0.0.1", "x23-invalid-service")();
  ASSERT_FALSE(invalid_service);
  EXPECT_EQ(invalid_service.error().code().category(), gai_category());

  // Pick an unused port, then release it before testing connection refusal.
  UniqueFd refusing(socket(AF_INET, SOCK_STREAM, 0));
  ASSERT_TRUE(refusing);
  ASSERT_EQ(bind(refusing.get(), reinterpret_cast<sockaddr *>(&*address),
                 sockaddr_len(*address)),
            0);
  auto refused_address = local_addr(refusing);
  // Darwin drops SYNs to bound, non-listening sockets instead of refusing.
  refusing.reset();
  auto failed = cp.tcpconnect(&refused_address)();
  ASSERT_FALSE(failed);
  EXPECT_EQ(failed.error(), ECONNREFUSED);
  auto port =
      ntohs(reinterpret_cast<sockaddr_in *>(&refused_address)->sin_port);
  auto failed_host = cp.tcpconnect("127.0.0.1", std::to_string(port))();
  ASSERT_FALSE(failed_host);
  EXPECT_EQ(failed_host.error().code().value(), ECONNREFUSED);

  sockaddr unsupported{};
  unsupported.sa_family = AF_UNSPEC;
  EXPECT_FALSE(cp.tcpconnect(&unsupported, sizeof(unsupported))());
}

#if USE_EPOLL
TEST(EventEdges, UnpollableDescriptorFailsWaitAndCanBeReset)
{
  ThreadPool pool(8);
  UniqueFd fd(open("/dev/null", O_RDONLY | O_CLOEXEC));
  ASSERT_TRUE(fd);
  ManualPoll cp(pool);
  auto token = cp.read_clear(fd);
  EXPECT_EQ(cp.read_wait(token)(), EBADF);
  auto generation = cp.generation(fd);
  cp.poison(fd, generation);
  EXPECT_EQ(cp.generation(fd), generation);
  cp.reset(fd);
  generation = cp.generation(fd);
  cp.poison(fd, generation);
  EXPECT_EQ(cp.generation(fd), generation);
}

TEST(EventEdges, SignalDumpIncludesPendingReadAndWrite)
{
  ThreadPool pool(8);
  SocketPair sockets;
  ManualPoll cp(pool);
  auto reading = cp.read_wait(cp.read_clear(sockets.a));
  auto writing = cp.write_wait(cp.write_clear(sockets.a));
  std::binary_semaphore dumped(0);
  AutoSignalCbHandle done = signal_cb(SIGUSR1, [&] { dumped.release(); });
  testing::internal::CaptureStderr();
  signal_handler(SIGUSR1);
  bool finished = dumped.try_acquire_for(seconds(5));
  auto output = testing::internal::GetCapturedStderr();
  EXPECT_TRUE(finished);
  EXPECT_NE(output.find("x23::CoroPoll"), std::string::npos);
  EXPECT_NE(output.find("reader"), std::string::npos);
  EXPECT_NE(output.find("writer"), std::string::npos);
  cp.poison(sockets.a);
  EXPECT_EQ(reading(), EBADF);
  EXPECT_EQ(writing(), EBADF);
}
#endif // USE_EPOLL

TEST(EventEdges, WriteCompletesAndDelayedFlushReportsBrokenPipe)
{
  ThreadPool pool(8);
  SocketPair sockets;
  CoroPoll cp(pool);
  Buf bytes{'a', 'b', 'c'};
  EXPECT_EQ(cp.write(sockets.a, bytes)().value(), bytes.size());
  EXPECT_EQ(cp.write(sockets.a, BufView{})().value(), 0UZ);
  std::array<char, 3> out{};
  ASSERT_EQ(::read(sockets.b.get(), out.data(), out.size()), 3);
  EXPECT_EQ(std::string_view(out.data(), out.size()), "abc");
  sockets.b.reset();
  EXPECT_EQ(cp.dwrite(sockets.a,
                      Buf(detail::WriteBuffer::kFlushThreshold, 0xa5)),
            EPIPE);
}

TEST(EventEdges, AccumulatedDelayedWritesFlushAtThreshold)
{
  ThreadPool pool(8);
  SocketPair sockets;
  CoroPoll cp(pool);
  constexpr auto kHalf = detail::WriteBuffer::kFlushThreshold / 2;
  EXPECT_EQ(cp.dwrite(sockets.a, Buf(kHalf, 'a')), 0);
  char byte;
  ASSERT_EQ(::read(sockets.b.get(), &byte, 1), -1);
  EXPECT_EQ(errno, EAGAIN);
  EXPECT_EQ(cp.dwrite(sockets.a, Buf(kHalf, 'b')), 0);
  Buf received;
  EXPECT_EQ(cp.read(sockets.b, &received, 2 * kHalf)().value(), 2 * kHalf);
  EXPECT_TRUE(std::ranges::all_of(received.subspan(0, kHalf),
                                  [](auto c) { return c == 'a'; }));
  EXPECT_TRUE(std::ranges::all_of(received.subspan(kHalf),
                                  [](auto c) { return c == 'b'; }));
}

TEST(EventEdges, AcceptRejectsNonListeningSocket)
{
  ThreadPool pool(8);
  SocketPair sockets;
  CoroPoll cp(pool);
  auto result = cp.accept(sockets.a)();
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error(), EINVAL);
}

TEST(EventEdges, OnDemandPointerMoveAndResetPreserveHeldReferences)
{
  detail::OnDemandSharedPtr<detail::ReadBuffer> original;
  auto held = original.get();
  EXPECT_EQ(original.get().get(), held.get());
  auto moved = std::move(original);
  EXPECT_EQ(moved.get().get(), held.get());
  auto &alias = moved;
  moved = std::move(alias);
  EXPECT_EQ(moved.get().get(), held.get());
  moved.reset();
  EXPECT_NE(moved.get().get(), held.get());
  EXPECT_NE(original.get().get(), held.get());
}
