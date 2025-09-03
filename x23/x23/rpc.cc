
#include <memory>
#include <sys/socket.h>

#include "x23/rpc.h"
#include "x23/sockio.h"

namespace x23::detail {

RpcStreamImpl::RpcStreamImpl(CoroPoll &cp, UniqueFd fd)
  : cp_(cp), fd_(std::move(fd))
{
  set_nonblock(fd_);
}

RpcStreamImpl::~RpcStreamImpl()
{
  cp_->reset(fd_.get());
}

void
RpcStreamImpl::fail()
{
  if (ss_.stop_requested())
    return;
  ss_.request_stop();
  cp_->poison(fd_.get());
  auto oldmap = std::exchange(cstate_->xidmap_, {});
  for (auto [_xid, cb] : oldmap)
    cb(Buf{});
}

Async<std::expected<std::uint32_t, Errno>>
RpcStreamImpl::read_uint(SLOC_ARG0)
{
  Buf b;
  if (co_await co_await cp_->read(fd_, &b, 4, true, true) != 4)
    co_return Errno{ENOMSG};
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
    done = len & 0x80000000;
    len &= 0x7fffffff;
    if (bp->size() + len > max_record_size)
      co_return Errno{ENOMSG};
    if (auto res = co_await cp_->read(fd_, bp, len, true, true); !res)
      co_return res.error();
    else if (*res != len)
      co_return Errno{ENOMSG};
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
  constexpr size_t kMinBuf = 512;
  RefPtr<RpcStreamImpl> _r = *this;
  Buf b;
  b.reserve(kMinBuf);
  while (!ss_.stop_requested()) {
    if (Errno err = co_await read_record(&b)) {
      fail();
      co_return;
    }
    rpc_msg msg;
    size_t hdrbytes;
    try {
      hdrbytes = deserialize_from(&msg, b);
    } catch (XdrException &) {
      fail();
      break;
    }
    if (msg.body.mtype() == REPLY) {
      auto cs = cstate_.take();
      auto it = cs->xidmap_.find(msg.xid);
      if (it == cs->xidmap_.end())
        continue;
      auto cb = it->second;
      cs->xidmap_.erase(it);
      cs.reset();
      cb(std::move(b));
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
