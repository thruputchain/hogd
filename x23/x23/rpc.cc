
#include <memory>
#include <sys/socket.h>

#include "x23/rpc.h"

namespace x23 {

RpcLogger::RpcLogger(int peerfd, int logfd) noexcept : logfd_(logfd)
{
  // XXX - use getsockname to show something more useful?
  peername_ = std::format("fd {}", peerfd);
}

std::string
msg_show_stat(const rpc_msg &msg)
{
  using enum accept_stat;
  using enum reject_stat;
  using enum msg_type;
  using enum reply_stat;
  if (msg.body.mtype() != REPLY) [[unlikely]]
    return "CALL";
  if (msg.body.rbody().stat() == MSG_ACCEPTED) [[likely]] {
    if (accept_stat stat = msg.body.rbody().areply().reply_data.stat();
        stat == PROG_MISMATCH)
      return std::format(
          "PROG_MISMATCH[low={},high={}]",
          msg.body.rbody().areply().reply_data.mismatch_info().low,
          msg.body.rbody().areply().reply_data.mismatch_info().high);
    else
      return std::format("{}", stat);
  }
  else if (reject_stat stat = msg.body.rbody().rreply().stat();
           stat == AUTH_ERROR)
    return std::format("{}", msg.body.rbody().rreply().rj_why());
  else
    return std::format("{}", stat);
}

namespace detail {
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
RpcStreamImpl::write_record(Buf b)
{
  if (ss_.stop_requested())
    co_return;
  auto _r = shared_from_this();
  CoroGuard g;
  co_await g(sendlock_);
  if (!ss_.stop_requested())
    co_await cp_->write(fd_, b);
  co_await g.reset_and_return();
}

Async<std::expected<std::uint32_t, Errno>>
RpcStreamImpl::read_uint()
{
  Buf b;
  if (co_await co_await cp_->read(fd_, &b, 4) != 4)
    co_return Errno{ENOMSG};
  co_return getbe<std::uint32_t>(b.data());
}

Async<std::expected<Buf, Errno>>
RpcStreamImpl::read_record()
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
RpcStreamImpl::gen_call_xid(CoroCB<Buf> cb)
{
  auto cs = cstate_.take();
  if (ss_.stop_requested())
    throw XdrException("RPC call on failed file descriptor");
  for (;;) {
    std::uint32_t xid = ++cs->lastxid_;
    if (auto r = cs->xidmap_.emplace(xid, cb); r.second) [[likely]]
      return xid;
  }
}

Detached
RpcStreamImpl::reader()
{
  using enum msg_type;
  auto _r = shared_from_this();
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
    else if (msg.body.mtype() == CALL)
      serve_call(msg, b.subspan(hdrbytes));
  }
}

Detached
RpcStreamImpl::serve_call(const rpc_msg &call, BufView args)
{
  using enum accept_stat;
  using enum reject_stat;
  auto _r = shared_from_this();
  const call_body &cb = call.body.cbody();
  rpc_msg res{.xid = call.xid};

  if (cb.rpcvers != 2) [[unlikely]] {
    msg_set_reply_stat(&res, RPC_MISMATCH);
    send_msg(res);
    co_return;
  }

  auto it = srvmap_.find(ProgVers{cb.prog, cb.vers});
  if (it == srvmap_.end()) {
    it = srvmap_.lower_bound(ProgVers{cb.prog, 0});
    if (it == srvmap_.end() || it->first.prog != cb.prog)
      res.body.rbody().areply().reply_data.stat(PROG_UNAVAIL);
    else {
      res.body.rbody().areply().reply_data.stat(PROG_MISMATCH);
      auto &mmi = res.body.rbody().areply().reply_data.mismatch_info();
      mmi.low = it->first.vers;
      it = srvmap_.upper_bound(ProgVers{cb.prog, 0xffffffff});
      mmi.high = (--it)->first.vers;
    }
    send_msg(res);
    co_return;
  }

  RpcService *s = it->second.get();
  Buf resp = co_await s->serve(call, args);
  write_record(resp);
  co_return;
}
} // namespace detail
} // namespace x23
