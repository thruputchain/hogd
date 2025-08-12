
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

Detached
RpcStreamImpl::write_record(Buf b SLOC_ARG)
{
  if (ss_.stop_requested())
    co_return;
  RefPtr<RpcStreamImpl> _r = *this;
  if (!ss_.stop_requested())
    co_await cp_->bgwrite(fd_, b);
}

Async<std::expected<std::uint32_t, Errno>>
RpcStreamImpl::read_uint(SLOC_ARG0)
{
  Buf b;
  if (co_await co_await cp_->read(fd_, &b, 4) != 4)
    co_return Errno{ENOMSG};
  co_return getbe<std::uint32_t>(b.data());
}

Async<std::expected<Buf, Errno>>
RpcStreamImpl::read_record(SLOC_ARG0)
{
  Buf b;
  bool done = false;
  while (!done) {
    std::uint32_t len = co_await co_await read_uint();
    done = len & 0x80000000;
    len &= 0x7fffffff;
    if (b.size() + len > max_record_size)
      co_return Errno{ENOMSG};
    if (co_await co_await cp_->read(fd_, &b, len) != len)
      co_return Errno{ENOMSG};
  }
  co_return b;
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
  RefPtr<RpcStreamImpl> _r = *this;
  Buf b;
  while (!ss_.stop_requested()) {
    if (auto r = co_await read_record(); !r) {
      fail();
      co_return;
    }
    else
      b = std::move(*r);
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
    }
    else if (msg.body.mtype() == CALL) {
      // serve_call(msg, b.subspan(hdrbytes));
      rpc_msg res;
      if (RpcService *s = srvmap_.lookup(msg, &res)) {
	Buf resp = co_await s->serve(msg, b.subspan(hdrbytes));
	write_record(resp);
      }
      else
	send_msg(res);
    }
  }
}

/*
Detached
RpcStreamImpl::serve_call(const rpc_msg &call, BufView args SLOC_ARG)
{
  RefPtr<RpcStreamImpl> _r = *this;
  rpc_msg res;
  if (RpcService *s = srvmap_.lookup(call, &res)) {
    Buf resp = co_await s->serve(call, args);
    write_record(resp);
  }
  else
    send_msg(res);
  co_return;
}
*/

} // namespace x23::detail
