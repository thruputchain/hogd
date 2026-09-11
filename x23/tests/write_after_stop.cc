// Regression test for the stop_requested() divergence between the poll and
// epoll backends of CoroPollImpl::get_reader/get_writer/get_rw.
//
// CoroPoll's convention is that once a stop has been requested, operations on
// a file descriptor report EBADF rather than pretending to succeed.  read(),
// connect() and both tcpconnect() overloads state it directly, each opening
// with
//
//     if (stop_requested())
//       co_return Errno{EBADF};
//
// awrite() and dwrite() have no such guard of their own.  They rely on
// get_writer() returning nullptr:
//
//     auto w = get_writer(fd);
//     if (!w)
//       return EBADF;
//
// The poll backend makes that hold -- #3313 added an explicit
// `if (stop_requested()) return nullptr;` to get_reader, get_writer and
// get_rw.  The epoll backend never got the same guard; it returns nullptr only
// for a fd in kPoisoned mode.  A fd that is merely kActive or kFree (which is
// what ~CoroPoll's reset sweep leaves behind) still yields a freshly created
// WriteBuffer, so awrite()/dwrite() report success after a stop has been
// requested.  dwrite() then parks the payload in a buffer that nothing will
// ever flush, which is a silent write loss rather than an error the caller can
// see.
//
// This matters because ~CoroPoll expects exactly these late calls: it notes
// that the reset sweep wakes coroutines with EBADF and that any poison/reset
// they make in response "will immediately return without doing anything since
// stop_requested() is true".  A woken coroutine that writes instead of
// poisoning gets an error on macOS and silence on Linux.
//
// Expected: RED on epoll (Linux), green on poll (macOS/BSD).  The fix is the
// same three-line guard #3313 added to event_impl_poll.cc.

#include <cerrno>
#include <cstdint>

#include <sys/socket.h>

#include <gtest/gtest.h>

#include "x23/err.h"
#include "x23/event.h"
#include "x23/sockio.h"
#include "x23/threadpool.h"
#include "x23/vec.h"

using namespace x23;

namespace {

// Same shape as event_edges.cc's ManualPoll: no poll thread, and
// request_stop() reachable from the test.
class ManualPoll : public CoroPoll {
public:
  explicit ManualPoll(ThreadPool &pool) : CoroPoll(pool, no_loop_thread{}) {}
  using CoroPoll::request_stop;
};

struct Fixture {
  ThreadPool pool{1};
  ManualPoll cp{pool};
  UniqueFd a, b;

  Fixture()
  {
    int fds[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds))
      Errno{}.raise("socketpair");
    a.reset(fds[0]);
    b.reset(fds[1]);
    set_nonblock(a);
    set_nonblock(b);
    cp.request_stop();
  }

  static Buf payload() { return Buf(64, std::uint8_t{0xc7}); }
};

} // namespace

// The baseline: read() checks stop_requested() itself, so it reports EBADF on
// both backends.  If this ever fails the convention has changed and the two
// expectations below should be revisited rather than the backends.
TEST(WriteAfterStop, ReadReportsEbadf)
{
  Fixture f;
  Buf out;
  auto r = f.cp.read(f.a, &out, 1, /*full=*/false, /*sync=*/false)();
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error().raw_code(), EBADF);
}

// Delayed write.  On epoll this returns 0 and strands the payload in a
// WriteBuffer that no poll loop will ever service.
TEST(WriteAfterStop, DelayedWriteReportsEbadf)
{
  Fixture f;
  EXPECT_EQ(f.cp.dwrite(f.a, Fixture::payload()).raw_code(), EBADF);
}

// Immediate write.  On epoll this returns 0 as well.
TEST(WriteAfterStop, AsyncWriteReportsEbadf)
{
  Fixture f;
  EXPECT_EQ(f.cp.awrite(f.a, Fixture::payload()).raw_code(), EBADF);
}
