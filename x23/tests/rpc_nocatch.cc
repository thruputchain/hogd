// Coverage for RpcStream::nocatch_server (x23/rpc.h).
//
// The ordinary server() registration wraps every dispatch in a
// catch(std::exception &) that turns a failure into an RPC error reply:
// GARBAGE_ARGS while the arguments are still being decoded, SYSTEM_ERR once
// the handler itself is running.  The peer sees a failed call and the
// process carries on.
//
// nocatch_server() suppresses that catch, so the exception unwinds into the
// coroutine machinery instead, which prints "Unhandled exception in
// coroutine" with a stack trace and then terminates.  The point is to fault
// where the bug is, with a core dump, rather than to hand the caller an
// error reply and keep running on state nobody has reasoned about.
//
// Each behavior below is exercised against both registrations, because what
// matters about the flag is the difference between the two.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <stdexcept>

#include <sys/socket.h>
#include <unistd.h>

#include <gtest/gtest.h>

#include "tests/xdrtest.hh"
#include "x23/async.h"
#include "x23/cleanup.h"
#include "x23/corosync.h"
#include "x23/err.h"
#include "x23/event.h"
#include "x23/rpc.h"
#include "x23/rpc_common.h"
#include "x23/serialize.h"
#include "x23/threadpool.h"
#include "x23/vec.h"

namespace x23 {
namespace {

using EchoMeta = blank_rpc_v1::proc_meta<1>;

// Also used verbatim as the death-test regex, so keep it free of characters
// a POSIX extended regex would read as operators.
constexpr const char kBoom[] = "rpc nocatch test boom";

// Fails the way a buggy handler fails.  Written as a value-returning
// function so a coroutine handler can end in a co_return instead of falling
// off the end of a body whose promise has no return_void.
Data
boom()
{
  throw std::runtime_error(kBoom);
}

// Throws without ever suspending, so the exception unwinds through
// RpcDispatcher::ProcHandler::operator() while it is still on the stack.
struct ThrowsImmediately {
  using rpc_service = blank_rpc_v1;
  static void null() {}
  static Data echo(Data /*arg*/) { return boom(); }
};
static_assert(is_blank_rpc_v1<ThrowsImmediately *, MaybeAsync>);

// Parks until a second call on the same connection releases it, and only
// then throws.  The reader loop reads one record at a time, so the releasing
// null() cannot even be read until after the dispatcher has seen that echo
// did not finish synchronously and has moved it onto the Async<Buf>
// continuation.  That ordering is what makes this a different catch site
// from ThrowsImmediately rather than a slower version of the same one.
class ThrowsAfterSuspending {
public:
  using rpc_service = blank_rpc_v1;

  explicit ThrowsAfterSuspending(ThreadPool &pool) : pool_(&pool) {}

  Async<void> null()
  {
    released_.store(true);
    released_.broadcast(pool_);
    co_return;
  }

  Async<Data> echo(Data /*arg*/)
  {
    while (!released_.load())
      co_await released_.wait(false);
    co_return boom();
  }

private:
  ThreadPool *pool_;
  CoroFutex<bool> released_ = false;
};
static_assert(is_blank_rpc_v1<ThrowsAfterSuspending *, MaybeAsync>);

struct NeverThrows {
  using rpc_service = blank_rpc_v1;
  static Async<void> null() { co_return; }
  static Async<Data> echo(Data arg) { co_return arg; }
};
static_assert(is_blank_rpc_v1<NeverThrows *, MaybeAsync>);

// A CALL record for blank_rpc_v1::echo whose argument bytes are not a
// decodable Data: an opaque<> length prefix of 0xffffffff with nothing
// behind it.  A typed client cannot produce this, and it is the one failure
// the catching server answers with GARBAGE_ARGS rather than SYSTEM_ERR --
// the failure that comes from the peer instead of from the handler.
Buf
undecodable_echo_record(std::uint32_t xid)
{
  auto msg = call_msg<EchoMeta>();
  msg.xid = xid;
  return make_record(msg, std::uint32_t{0xffffffff});
}

// Fatal on the shape checks: reply_data is a union arm, so reading it on
// anything but an accepted reply throws, and throwing out of a catch block
// in a test that is already failing loses the diagnostic.
void
expect_reply_status(const rpc_msg &m, accept_stat want)
{
  ASSERT_EQ(m.body.mtype(), msg_type::REPLY);
  ASSERT_EQ(m.body.rbody().stat(), reply_stat::MSG_ACCEPTED);
  EXPECT_EQ(m.body.rbody().areply().reply_data.stat(), want);
}

void
write_record(int fd, const Buf &record)
{
  ASSERT_EQ(::write(fd, record.data(), record.size()),
            static_cast<ssize_t>(record.size()));
}

void
read_reply(int fd, rpc_msg *reply)
{
  std::uint8_t buf[512];
  ssize_t n = ::read(fd, buf, sizeof buf);
  ASSERT_GT(n, 4) << "no RPC reply came back";
  deserialize_from(reply, BufView(buf + 4, static_cast<std::size_t>(n) - 4));
}

UniqueFd
socketpair_or_raise(int *server_fd)
{
  int fds[2];
  if (::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) < 0)
    Errno{}.raise("socketpair");
  *server_fd = fds[1];
  return UniqueFd{fds[0]};
}

} // namespace

TEST(RpcNoCatch, CatchingServerReportsSynchronousThrow)
{
  ThrowsImmediately impl;
  ThreadPool pool(2);
  CoroPoll cp(pool);

  int server_fd = kInvalidFd;
  UniqueFd client_fd = socketpair_or_raise(&server_fd);
  rpc_stream(cp, server_fd)->server(&impl);
  auto client = rpc_stream(cp, client_fd.release())->client<blank_rpc_v1>();

  try {
    (void)client.echo(Data{1, 2, 3})();
    ADD_FAILURE() << "echo returned instead of reporting the handler failure";
  } catch (const XdrCallFailed &e) {
    expect_reply_status(e.m_, accept_stat::SYSTEM_ERR);
  }

  // A handler bug costs one call, not the connection.
  EXPECT_NO_THROW(client.null()());
}

TEST(RpcNoCatch, CatchingServerReportsThrowAfterSuspension)
{
  ThreadPool pool(2);
  ThrowsAfterSuspending impl(pool);
  CoroPoll cp(pool);

  int server_fd = kInvalidFd;
  UniqueFd client_fd = socketpair_or_raise(&server_fd);
  rpc_stream(cp, server_fd)->server(&impl);
  auto client = rpc_stream(cp, client_fd.release())->client<blank_rpc_v1>();

  auto pending = client.echo(Data{1, 2, 3}); // parks inside the handler
  client.null()();                           // releases it; it then throws

  try {
    (void)pending();
    ADD_FAILURE() << "echo returned instead of reporting the handler failure";
  } catch (const XdrCallFailed &e) {
    expect_reply_status(e.m_, accept_stat::SYSTEM_ERR);
  }
}

TEST(RpcNoCatch, CatchingServerReportsUndecodableArguments)
{
  NeverThrows impl;
  ThreadPool pool(2);
  CoroPoll cp(pool);

  int server_fd = kInvalidFd;
  UniqueFd client_fd = socketpair_or_raise(&server_fd);
  rpc_stream(cp, server_fd)->server(&impl);

  ASSERT_NO_FATAL_FAILURE(
      write_record(client_fd.get(), undecodable_echo_record(77)));
  rpc_msg reply;
  ASSERT_NO_FATAL_FAILURE(read_reply(client_fd.get(), &reply));
  EXPECT_EQ(reply.xid, 77U);
  expect_reply_status(reply, accept_stat::GARBAGE_ARGS);
}

// nocatch_server() changes only what happens when a dispatch throws; a
// working call is served exactly as server() would serve it.  Both
// spellings are covered: the VersMeta-deduced one, and the explicit one
// that hogd's RpcServer uses.
TEST(RpcNoCatch, NoCatchServerServesSuccessfulCalls)
{
  NeverThrows impl;
  ThreadPool pool(2);
  CoroPoll cp(pool);

  int server_fd = kInvalidFd;
  UniqueFd client_fd = socketpair_or_raise(&server_fd);
  rpc_stream(cp, server_fd)->nocatch_server(&impl);
  auto client = rpc_stream(cp, client_fd.release())->client<blank_rpc_v1>();

  EXPECT_NO_THROW(client.null()());
  EXPECT_EQ(client.echo(Data{4, 5, 6})(), (Data{4, 5, 6}));
}

TEST(RpcNoCatch, NoCatchServerAcceptsExplicitVersMeta)
{
  NeverThrows impl;
  ThreadPool pool(2);
  CoroPoll cp(pool);

  int server_fd = kInvalidFd;
  UniqueFd client_fd = socketpair_or_raise(&server_fd);
  rpc_stream(cp, server_fd)->nocatch_server<blank_rpc_v1>(&impl);
  auto client = rpc_stream(cp, client_fd.release())->client<blank_rpc_v1>();

  EXPECT_EQ(client.echo(Data{7, 8, 9})(), (Data{7, 8, 9}));
}

namespace {

// The death-test child is where all of this has to be built: a fork would
// not carry the thread pool's threads across, so nothing may be set up in
// the parent.  Each scenario is a function rather than an inline block so
// the EXPECT_DEATH macro is not handed a statement full of commas.

// Reached only when the server turned the failure into a reply instead of
// dying.  Returning from the scenario is how gtest gets to report that
// nothing died; letting the exception escape would terminate the child and
// pass the death test for the wrong reason.
void
reply_instead_of_crash()
{
  std::fputs("the failure came back as an rpc reply\n", stderr);
}

void
run_synchronous_throw()
{
  ThrowsImmediately impl;
  ThreadPool pool(2);
  CoroPoll cp(pool);

  int server_fd = kInvalidFd;
  UniqueFd client_fd = socketpair_or_raise(&server_fd);
  rpc_stream(cp, server_fd)->nocatch_server(&impl);
  auto client = rpc_stream(cp, client_fd.release())->client<blank_rpc_v1>();

  try {
    (void)client.echo(Data{1, 2, 3})();
  } catch (const std::exception &) {
    reply_instead_of_crash();
  }
}

void
run_throw_after_suspension()
{
  ThreadPool pool(2);
  ThrowsAfterSuspending impl(pool);
  CoroPoll cp(pool);

  int server_fd = kInvalidFd;
  UniqueFd client_fd = socketpair_or_raise(&server_fd);
  rpc_stream(cp, server_fd)->nocatch_server(&impl);
  auto client = rpc_stream(cp, client_fd.release())->client<blank_rpc_v1>();

  auto pending = client.echo(Data{1, 2, 3});
  client.null()();
  try {
    (void)pending();
  } catch (const std::exception &) {
    reply_instead_of_crash();
  }
}

void
run_undecodable_arguments()
{
  NeverThrows impl;
  ThreadPool pool(2);
  CoroPoll cp(pool);

  int server_fd = kInvalidFd;
  UniqueFd client_fd = socketpair_or_raise(&server_fd);
  rpc_stream(cp, server_fd)->nocatch_server(&impl);

  Buf record = undecodable_echo_record(77);
  if (::write(client_fd.get(), record.data(), record.size()) < 0)
    Errno{}.raise("write");

  // Never returns once the server dies with the process.  A server that
  // answered instead sends a GARBAGE_ARGS reply, and returning from here is
  // how gtest gets to report that nothing died.
  std::uint8_t buf[64];
  if (::read(client_fd.get(), buf, sizeof buf) < 0)
    Errno{}.raise("read");
}

} // namespace

TEST(RpcNoCatchDeathTest, SynchronousThrowTerminatesProcess)
{
  GTEST_FLAG_SET(death_test_style, "threadsafe");
  EXPECT_DEATH(run_synchronous_throw(), kBoom);
}

TEST(RpcNoCatchDeathTest, ThrowAfterSuspensionTerminatesProcess)
{
  GTEST_FLAG_SET(death_test_style, "threadsafe");
  EXPECT_DEATH(run_throw_after_suspension(), kBoom);
}

// Worth stating outright: under nocatch_server a peer that sends arguments
// the server cannot decode kills the process.  That is a much wider blast
// radius than a handler bug -- the failure is the peer's, not the server's
// -- and it is the reason nocatch_server belongs on links whose other end
// is trusted, not on anything a stranger can connect to.
TEST(RpcNoCatchDeathTest, UndecodableArgumentsTerminateProcess)
{
  GTEST_FLAG_SET(death_test_style, "threadsafe");
  EXPECT_DEATH(run_undecodable_arguments(), "Unhandled exception in coroutine");
}

} // namespace x23
