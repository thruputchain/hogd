#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <memory>
#include <mutex>
#include <semaphore>
#include <string>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

#include <gtest/gtest.h>

#include "tests/xdrtest.hh"
#include "x23/fileio.h"
#include "x23/srpc.h"

using namespace x23;

namespace {

// Only this executable substitutes Unix sockets for TCP. SRPC still uses
// its real reader/writer threads, record parser, queues, and callbacks.
thread_local bool local_connect{};
thread_local int connect_error{};
thread_local std::vector<UniqueFd> accepted;
std::atomic<int> read_fd{-1}, read_error{}, read_limit{};
std::atomic<int> write_fd{-1}, write_error{};

// A wake descriptor models shutdown even when the host forbids that syscall.
// Both endpoints observe EOF, and blocked reads wake without closing live fds.
std::mutex transport_mutex;
std::unordered_map<int, std::shared_ptr<UniqueFd>> transport_wakes;

std::shared_ptr<UniqueFd>
transport_wake(int fd)
{
  std::lock_guard lock(transport_mutex);
  auto it = transport_wakes.find(fd);
  return it == transport_wakes.end() ? nullptr : it->second;
}

struct Srpc : testing::Test {
  void TearDown() override
  {
    std::lock_guard lock(transport_mutex);
    transport_wakes.clear();
  }
};

struct SocketPair {
  UniqueFd stream, peer;
  SocketPair()
  {
    int fds[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds))
      Errno{}.raise("socketpair");
    stream.reset(fds[0]);
    peer.reset(fds[1]);
    auto wake =
        std::make_shared<UniqueFd>(eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK));
    if (!*wake)
      Errno{}.raise("eventfd");
    std::lock_guard lock(transport_mutex);
    transport_wakes[fds[0]] = wake;
    transport_wakes[fds[1]] = std::move(wake);
  }
};

struct EchoServer {
  using rpc_service = blank_rpc_v1;
  void null() {}
  Data echo(Data data) { return data; }
};

Buf
read_record(BorrowedFd fd)
{
  Buf marker, record;
  if (auto res = fullread(fd, &marker, 4); !res)
    res.error().raise("read marker");
  auto size = getbe<uint32_t>(marker.data());
  EXPECT_NE(size & 0x80000000U, 0U);
  if (auto res = fullread(fd, &record, size & 0x7fffffffU); !res)
    res.error().raise("read record");
  return record;
}

rpc_msg
read_call(BorrowedFd peer)
{
  auto bytes = read_record(peer);
  rpc_msg msg;
  deserialize_from(&msg, bytes);
  EXPECT_EQ(msg.body.mtype(), msg_type::CALL);
  return msg;
}

using EchoMeta = blank_rpc_v1::proc_meta<1>;
using NullMeta = blank_rpc_v1::proc_meta<0>;

struct RawStream : SRpcStream<> {
  using SRpcStream<>::SRpcStream;
  using SRpcFd::write_call;
};

} // namespace

extern "C" {
int __real_socket(int, int, int);
int __real_connect(int, const sockaddr *, socklen_t);
int __real_setsockopt(int, int, int, const void *, socklen_t);
int __real_shutdown(int, int);
ssize_t __real_read(int, void *, size_t);
ssize_t __real_writev(int, const iovec *, int);

int
__wrap_socket(int family, int type, int protocol)
{
  if (!local_connect)
    return __real_socket(family, type, protocol);
  SocketPair pair;
  accepted.push_back(std::move(pair.peer));
  return pair.stream.release();
}

int
__wrap_connect(int fd, const sockaddr *addr, socklen_t len)
{
  if (!local_connect)
    return __real_connect(fd, addr, len);
  if (connect_error) {
    errno = connect_error;
    return -1;
  }
  return 0;
}

int
__wrap_setsockopt(int fd, int level, int option, const void *value,
                  socklen_t size)
{
  if (level == IPPROTO_TCP && option == TCP_NODELAY && transport_wake(fd))
    return 0; // Every SRPC descriptor in this executable is a Unix socket.
  return __real_setsockopt(fd, level, option, value, size);
}

ssize_t
__wrap_read(int fd, void *buf, size_t size)
{
  if (fd == read_fd) {
    if (int error = read_error.exchange(0)) {
      errno = error;
      return -1;
    }
    if (int limit = read_limit)
      size = std::min(size, size_t(limit));
  }
  if (auto wake = transport_wake(fd)) {
    pollfd fds[] = {{fd, POLLIN, 0}, {wake->get(), POLLIN, 0}};
    int n;
    do {
      n = poll(fds, 2, -1);
    } while (n < 0 && errno == EINTR);
    if (n < 0)
      return -1;
    if (fds[1].revents)
      return 0;
  }
  return __real_read(fd, buf, size);
}

int
__wrap_shutdown(int fd, int how)
{
  if (auto wake = transport_wake(fd)) {
    uint64_t one = 1;
    return ::write(wake->get(), &one, sizeof(one)) == sizeof(one) ? 0 : -1;
  }
  return __real_shutdown(fd, how);
}

ssize_t
__wrap_writev(int fd, const iovec *iov, int count)
{
  if (fd == write_fd)
    if (int error = write_error.exchange(0)) {
      errno = error;
      return -1;
    }
  return __real_writev(fd, iov, count);
}
}

TEST_F(Srpc, ConfiguratorsStartLoggedServerAndClient)
{
  SocketPair sockets;
  EchoServer impl;
  UniqueFd log(open("/dev/null", O_WRONLY | O_CLOEXEC));
  ASSERT_TRUE(log);
  auto map = CpuMap(Vec<int>(CpuSet::num_cpus(), 0));
  RefPtr<SRpcStream<RpcLogger>> server;
  {
    auto config = srpc_stream_logged(sockets.stream.release(), log.get());
    config.set_affinity(&map.cpuset(0));
    server = *config.operator->();
    config->server<blank_rpc_v1>(&impl);
  }
  {
    auto client = srpc_stream_logged(sockets.peer.release(), log.get())
                      ->client(blank_rpc_v1{});
    client.null();
    EXPECT_EQ(client.echo(Data{1, 2, 3}), (Data{1, 2, 3}));
  }
  server->stop();
}

TEST_F(Srpc, UnloggedRoundTripAndFragmentedReads)
{
  SocketPair sockets;
  EchoServer impl;
  read_fd = sockets.stream.get();
  read_limit = 4;
  Defer clear([] {
    read_fd = -1;
    read_limit = 0;
  });
  RefPtr<SRpcStream<>> server;
  {
    auto config = srpc_stream(sockets.stream.release());
    server = *config.operator->();
    config->server(&impl);
  }
  {
    auto client = srpc_stream(sockets.peer.release())->client(blank_rpc_v1{});
    client.null();
    EXPECT_EQ(client.echo(Data(100, 42)), Data(100, 42));
  }
  server->stop();
}

TEST_F(Srpc, ScriptedRepliesReachClientErrors)
{
  enum class Reply { kSuccess, kRejected, kTruncated, kVoidBytes, kEof };
  UniqueFd log(open("/dev/null", O_WRONLY | O_CLOEXEC));
  ASSERT_TRUE(log);
  for (auto mode : {Reply::kSuccess, Reply::kRejected, Reply::kTruncated,
                    Reply::kVoidBytes, Reply::kEof}) {
    SocketPair sockets;
    RefPtr stream(*new SRpcStream<RpcLogger>(std::move(sockets.stream),
                                             RpcLogger{-1, log.get()}));
    auto client = stream->client(blank_rpc_v1{});
    stream->go();
    std::jthread peer([fd = std::move(sockets.peer), mode] {
      auto msg = read_call(fd);
      if (mode == Reply::kEof)
        return;
      msg_set_reply_stat(&msg, mode == Reply::kRejected
                                   ? accept_stat::PROC_UNAVAIL
                                   : accept_stat::SUCCESS);
      auto record = mode == Reply::kSuccess    ? make_record(msg, Data{8, 9})
                    : mode == Reply::kRejected ? make_record(msg)
                                               : make_record(msg, uint32_t{8});
      EXPECT_TRUE(fullwrite(fd, record));
    });
    if (mode == Reply::kSuccess)
      EXPECT_EQ(client.echo(Data{1}), (Data{8, 9}));
    else if (mode == Reply::kRejected) {
      try {
        client.echo(Data{});
        FAIL() << "error reply accepted";
      } catch (const XdrCallFailed &error) {
        EXPECT_EQ(error.m_.body.rbody().areply().reply_data.stat(),
                  accept_stat::PROC_UNAVAIL);
        EXPECT_NE(std::string(error.what()).find("echo"), std::string::npos);
      }
    }
    else if (mode == Reply::kVoidBytes)
      EXPECT_THROW(client.null(), XdrException);
    else
      EXPECT_THROW(client.echo(Data{}), XdrException);
    stream->stop();
  }
}

namespace {

template<typename T> struct FailingAllocator {
  using value_type = T;
  T *allocate(size_t) { throw std::bad_alloc{}; }
  void deallocate(T *p, size_t n) { std::allocator<T>{}.deallocate(p, n); }
};

struct AllocationReply : EchoMeta {
  using return_type = Vec<uint8_t, 8, FailingAllocator<uint8_t>>;
};

} // namespace

TEST_F(Srpc, ReplyAllocationFailureIsTranslatedToXdrException)
{
  SocketPair sockets;
  UniqueFd log(open("/dev/null", O_WRONLY | O_CLOEXEC));
  RefPtr stream(*new SRpcStream<RpcLogger>(std::move(sockets.stream),
                                           RpcLogger{-1, log.get()}));
  stream->go();
  std::jthread peer([fd = std::move(sockets.peer)] {
    auto msg = read_call(fd);
    msg_set_reply_stat(&msg, accept_stat::SUCCESS);
    EXPECT_TRUE(fullwrite(fd, make_record(msg, Data{1})));
  });
  EXPECT_THROW(stream->send_call<AllocationReply>(Data{}), XdrException);
  stream->stop();
}

TEST_F(Srpc, InvalidRecordsCloseTheStream)
{
  for (auto marker : {0x00000010U, 0x80000100U, 0x80000004U}) {
    SocketPair sockets;
    RefPtr stream(*new SRpcStream<>(std::move(sockets.stream)));
    stream->max_message_size_ = 64;
    stream->go();
    auto bytes = serialize(marker, uint32_t{1});
    ASSERT_TRUE(fullwrite(sockets.peer, bytes));
    char byte;
    EXPECT_LE(::read(sockets.peer.get(), &byte, 1), 0);
    stream->stop();
  }
}

TEST_F(Srpc, ReadErrorsStopWorkers)
{
  for (int error : {EIO, ECONNRESET}) {
    SocketPair sockets;
    read_fd = sockets.stream.get();
    read_error = error;
    Defer clear([] {
      read_fd = -1;
      read_error = 0;
    });
    RefPtr stream(*new SRpcStream<>(std::move(sockets.stream)));
    stream->go();
    char byte;
    EXPECT_EQ(::read(sockets.peer.get(), &byte, 1), 0);
    EXPECT_EQ(read_error.load(), 0);
    stream->stop();
  }
}

TEST_F(Srpc, WriteErrorsCompletePendingCall)
{
  for (int error : {EIO, EPIPE, EBADF, ECONNRESET}) {
    SocketPair sockets;
    write_fd = sockets.stream.get();
    write_error = error;
    Defer clear([] {
      write_fd = -1;
      write_error = 0;
    });
    RefPtr stream(*new SRpcStream<>(std::move(sockets.stream)));
    auto client = stream->client(blank_rpc_v1{});
    stream->go();
    EXPECT_THROW(client.echo(Data{1}), XdrException);
    EXPECT_EQ(write_error.load(), 0);
    stream->stop();
  }
}

TEST_F(Srpc, QueuedAndSubsequentCallsCompleteWhenStopped)
{
  SocketPair sockets;
  RefPtr stream(*new RawStream(std::move(sockets.stream)));
  unsigned callbacks = 0;
  auto cb = [&](rpc_msg *msg, BufView bytes) {
    EXPECT_EQ(msg, nullptr);
    EXPECT_TRUE(bytes.empty());
    ++callbacks;
  };
  auto call = call_msg<NullMeta>();
  stream->write_call(make_record(call), cb);
  stream->write_call(make_record(call), cb);
  msg_set_reply_stat(&call, accept_stat::SUCCESS);
  stream->write_call(make_record(call), {});
  stream->stop();
  EXPECT_EQ(callbacks, 2U);
  stream->write_call(make_record(call_msg<NullMeta>()), cb);
  EXPECT_EQ(callbacks, 3U);
  stream->stop();
  EXPECT_EQ(callbacks, 3U);
}

TEST_F(Srpc, OversizedCallsAreRejectedBeforeQueueing)
{
  SocketPair sockets;
  RefPtr stream(*new SRpcStream<>(std::move(sockets.stream)));
  auto client = stream->client(blank_rpc_v1{});
  stream->max_message_size_ = 64;
  EXPECT_THROW(client.echo(Data(100, 42)), XdrBadInput);
}

TEST_F(Srpc, UnknownProgramsReceiveAnErrorReply)
{
  SocketPair sockets;
  RefPtr stream(*new SRpcStream<>(std::move(sockets.stream)));
  stream->go();
  auto call = call_msg<NullMeta>();
  call.xid = 123;
  ASSERT_TRUE(fullwrite(sockets.peer, make_record(call)));
  rpc_msg reply;
  deserialize(&reply, read_record(sockets.peer));
  EXPECT_EQ(reply.xid, call.xid);
  ASSERT_EQ(reply.body.mtype(), msg_type::REPLY);
  EXPECT_EQ(reply.body.rbody().areply().reply_data.stat(),
            accept_stat::PROG_UNAVAIL);
  stream->stop();
}

TEST_F(Srpc, DuplicateXidsAreReassignedAndUnknownRepliesIgnored)
{
  SocketPair sockets;
  RefPtr stream(*new RawStream(std::move(sockets.stream)));
  std::counting_semaphore<2> done(0);
  std::array<uint32_t, 2> replies{};
  auto call = call_msg<NullMeta>();
  call.xid = 50;
  for (size_t i = 0; i < replies.size(); ++i)
    stream->write_call(make_record(call), [&, i](rpc_msg *reply, BufView) {
      EXPECT_NE(reply, nullptr);
      if (reply)
        replies[i] = reply->xid;
      done.release();
    });
  stream->go();
  stream->go(); // Starting a running stream must not create more workers.
  auto first = read_call(sockets.peer);
  auto second = read_call(sockets.peer);
  EXPECT_EQ(first.xid, 50U);
  EXPECT_NE(first.xid, second.xid);
  auto unknown = first;
  unknown.xid = 12345;
  msg_set_reply_stat(&unknown, accept_stat::SUCCESS);
  msg_set_reply_stat(&first, accept_stat::SUCCESS);
  msg_set_reply_stat(&second, accept_stat::SUCCESS);
  auto records = make_record(unknown);
  records.append_range(make_record(second));
  records.append_range(make_record(first));
  ASSERT_TRUE(fullwrite(sockets.peer, records));
  done.acquire();
  done.acquire();
  EXPECT_EQ(replies[0], first.xid);
  EXPECT_EQ(replies[1], second.xid);
  stream->stop();
}

TEST_F(Srpc, MulticlientUsesConfiguredCpuBuckets)
{
  EchoServer impl;
  CpuMap map(Vec<int>(CpuSet::num_cpus(), 0));
  sockaddr_storage address{};
  address.ss_family = AF_INET;
  local_connect = true;
  Defer clear([] {
    local_connect = false;
    connect_error = 0;
    accepted.clear();
  });
  {
    auto multi = SRpcMulticlient<RpcNoLogger>::make(address, map);
    ASSERT_EQ(accepted.size(), 1UZ);
    RefPtr<SRpcStream<>> server;
    {
      auto config = srpc_stream(accepted.back().release());
      server = *config.operator->();
      config->server(&impl);
    }
    auto client = multi->client(blank_rpc_v1{});
    client.null();
    EXPECT_EQ(client.echo(Data{4, 5}), (Data{4, 5}));
    server->stop();
  }
  connect_error = ECONNREFUSED;
  EXPECT_THROW(SRpcMulticlient<RpcNoLogger>::make(address, map),
               std::system_error);
}
