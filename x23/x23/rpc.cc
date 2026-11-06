
#include <x23/rpc.h>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <sys/socket.h>
#include <utility>
#include <vector>

#include <x23/async.h>
#include <x23/cleanup.h>
#include <x23/corotrace.h>
#include <x23/debug.h>
#include <x23/err.h>
#include <x23/event.h>
#include <x23/refcnt.h>
#include <x23/rpc_msg.hh>
#include <x23/serialize.h>
#include <x23/sockio.h>
#include <x23/threadpool.h>
#include <x23/vec.h>
#include <x23/x23.h>

namespace x23::detail {

RpcStreamImpl::RpcStreamImpl(CoroPoll &cp, UniqueFd fd)
  : cp_(cp), fd_(std::move(fd))
{
  set_nonblock(fd_);
}

RpcStreamImpl::~RpcStreamImpl()
{
  cp_->reset(fd_);
}

void
RpcStreamImpl::fail()
{
  x23assert(refcount()); // Should be called holding a reference

  if (!ss_.request_stop())
    return;

  auto oldmap = std::exchange(cstate_->xidmap_, {});
  for (auto [_xid, cb] : oldmap)
    cb(Buf{});
  cp_->poison(fd_);
}

Async<std::expected<std::uint32_t, Errno>>
RpcStreamImpl::read_uint(SLOC_ARG0)
{
  Buf b;
  auto r = co_await cp_->read(fd_, &b, 4, true, true);
  if (!r) {
    debug("read_uint(fd {}): {}", get_fd(), r.error());
    co_return r.error();
  }
  if (*r != 4) {
    if (*r)
      debug("read_uint(fd {}): received only {} bytes", get_fd(), *r);
    co_return Errno{ENOMSG};
  }
  co_return getbe<std::uint32_t>(b.data());
}

Async<Errno>
RpcStreamImpl::read_record(Buf *bp SLOC_ARG)
{
  bp->clear();
  bool done = false;
  while (!done) {
    std::uint32_t len;
    if (auto res = co_await read_uint(); !res)
      co_return res.error();
    else
      len = *res;
    done = ((len & 0x80000000) != 0U);
    len &= 0x7fffffff;
    if (bp->size() + len > max_message_size) {
      // NOLINTNEXTLINE(readability-static-accessed-through-instance)
      debug.err(
          "RpcStreamImpl: fd {} received record size {} exceeds maximum {}",
          get_fd(), bp->size() + len, max_message_size);
      co_return Errno{ENOMSG};
    }
    if (auto res = co_await cp_->read(fd_, bp, len, true, true); !res) {
      debug("RpcStreamImpl: read(fd {}): {}", get_fd(), res.error());
      co_return res.error();
    }
    else if (*res != len) {
      // NOLINTNEXTLINE(readability-static-accessed-through-instance)
      debug.err("RpcStreamImpl: read(fd {}, n {}) returned {} bytes", get_fd(),
                len, *res);
      co_return Errno{ENOMSG};
    }
  }
  co_return 0;
}

std::uint32_t
RpcStreamImpl::gen_call_xid(Awaitable<Buf> &a)
{
  auto cs = cstate_.take();
  if (ss_.stop_requested())
    throw XdrException("RPC call on failed file descriptor");
  for (;;) {
    std::uint32_t xid = ++cs->lastxid_;
    if (auto r = cs->xidmap_.emplace(xid, a); r.second) [[likely]]
      return xid;
  }
}

Detached
RpcStreamImpl::reader(SLOC_ARG0)
{
  using enum msg_type;
  // Having some extra bytes in the buffer allows us to elide certain
  // bounds checks in deserialization.
  constexpr size_t kMinBuf = 0;
  RefPtr<RpcStreamImpl> _r = *this;
  Buf b;
  if constexpr (kMinBuf != 0U)
    b.reserve(kMinBuf);
  // Replies whose results are set but whose coroutines are not yet scheduled.
  // set_result_without_resuming() hands back a handle that nothing runs until
  // it is enqueued, so this must be drained on every exit from the loop.
  std::vector<ThreadPool::Job> ready;
  Defer flush_ready([this, &ready] {
    if (!ready.empty())
      cp_->pool_.go(std::move(ready));
  });
  while (!ss_.stop_requested()) {
    // Flush before a read that would suspend: replies that arrived together
    // are enqueued together, so ThreadPool::go() takes a runqueue lock once
    // for the group rather than once per reply, and a batch never sits
    // unstarted while this coroutine waits for the next record.
    if (!ready.empty() && cp_->nbuffered(fd_) < 4) {
      cp_->pool_.go(std::move(ready));
      ready.clear();
    }
    if (Errno err = co_await read_record(&b)) {
      fail();
      co_return;
    }
    rpc_msg msg;
    size_t hdrbytes;
    try {
      hdrbytes = deserialize_from(&msg, b);
    } catch (XdrException &e) {
      debug("RpcStreamImpl deserialization: {}", e.what());
      fail();
      break;
    }
    if (msg.body.mtype() == REPLY) {
      auto cs = cstate_.take();
      auto it = cs->xidmap_.find(msg.xid);
      if (it == cs->xidmap_.end())
        continue;
      auto cb = std::move(it->second);
      cs->xidmap_.erase(it);
      cs.reset();
      // Hand the client's continuation to the pool rather than running it
      // here, on the CPU hint xidmap_ already recorded for it.
      //
      // Resuming inline was the better trade when the comment this replaces
      // was written: a continuation was "deserialize the reply and wake a
      // blocked caller", and a scheduling hop dwarfed it. Callers now await
      // their RPCs, so a continuation runs until its own next call -- in
      // hogd's executor, the rest of a transaction -- and running that here
      // holds off every other client of this connection.
      if (CoroHandle h = cb->set_result_without_resuming(std::move(b)))
        ready.emplace_back(h, cb.cpuhint);
      if constexpr (kMinBuf != 0U)
        b.reserve(kMinBuf);
    }
    else if (msg.body.mtype() == CALL) {
      rpc_msg res;
      if (RpcService *s = srvmap_.lookup(msg, &res)) {
        auto awaitable = s->serve(msg, {b.subspan(hdrbytes), b});
        // Only spawn another coroutine if result not already available
        decltype(auto) awaiter = detail::get_awaiter(awaitable);
        if (awaiter.await_ready())
          cp_->dwrite(fd_, awaiter.await_resume());
        else
          [](RefPtr<RpcStreamImpl> self, auto awaitable) -> Detached {
            self->cp_->dwrite(self->fd_, co_await awaitable);
          }(*this, std::move(awaitable));
      }
      else
        send_msg(res);
    }
  }
}

} // namespace x23::detail
