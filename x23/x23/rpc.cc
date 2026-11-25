
#include <x23/rpc.h>

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <sys/socket.h>
#include <utility>

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
    cb(RecordRef{});
  cp_->poison(fd_);
}

void
RecvBuf::make_room(size_t need)
{
  const size_t pending = avail();
  if (chunk_) {
    const bool sole = chunk_->unique();
    if (space() >= std::max(need, kMinRead) && (pending || !sole))
      return;
    if (sole) {
      if (pending && rpos_)
        std::memmove(chunk_->data(), rptr(), pending);
      rpos_ = 0;
      wpos_ = pending;
      if (space() >= need)
        return;
    }
  }
  auto fresh = RecvChunk::make(std::max(pending + need, kChunkSize));
  if (pending)
    std::memcpy(fresh->data(), rptr(), pending);
  chunk_ = std::move(fresh);
  rpos_ = 0;
  wpos_ = pending;
}

std::uint32_t
RpcStreamImpl::gen_call_xid(Awaitable<RecordRef> &a)
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
  RecvBuf in;
  // Earlier fragments of a multi-fragment record.  We never send
  // these ourselves, but ONC RPC allows them.
  Buf frags;
  while (!ss_.stop_requested()) {
    // Read until the buffer holds a complete fragment.
    const size_t have = in.avail();
    const std::uint32_t marker = have < 4 ? 0 : getbe<std::uint32_t>(in.rptr());
    const size_t len = marker & ~kLastFragment;
    if (frags.size() + len > max_message_size) {
      // NOLINTNEXTLINE(readability-static-accessed-through-instance)
      debug.err(
          "RpcStreamImpl: fd {} received record size {} exceeds maximum {}",
          get_fd(), frags.size() + len, max_message_size);
      fail();
      co_return;
    }
    if (have < 4 + len) {
      in.make_room(4 + len - have);
      auto res = co_await cp_->read_some(fd_, in.wptr(), in.space());
      if (res && *res)
        in.commit(*res);
      else if (res || res.error() != EAGAIN) {
        if (!res)
          debug("RpcStreamImpl: read(fd {}): {}", get_fd(), res.error());
        else if (have || !frags.empty())
          // NOLINTNEXTLINE(readability-static-accessed-through-instance)
          debug.err("RpcStreamImpl: fd {} closed in the middle of a record",
                    get_fd());
        fail();
        co_return;
      }
      continue;
    }

    // The record stays where it is: b is a view of it, and a reply
    // keeps its chunk alive by reference.
    BufView b{in.rptr() + 4, len};
    const std::uint8_t *bufend = in.bufend();
    RefPtr<RecvChunk> whole{nullptr};
    in.consume(4 + len);
    if (!(marker & kLastFragment) || !frags.empty()) [[unlikely]] {
      frags.append(b.begin(), b.end());
      if (!(marker & kLastFragment))
        continue;
      whole = RecvChunk::make(frags.size());
      std::memcpy(whole->data(), frags.data(), frags.size());
      b = {whole->data(), frags.size()};
      bufend = b.data() + b.size();
      frags.clear();
    }
    const RefPtr<RecvChunk> &chunk = whole ? whole : in.chunk();

    rpc_msg msg;
    size_t hdrbytes;
    try {
      hdrbytes = deserialize_from(&msg, BufViewEnd{b, bufend});
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
      // Re-enqueing this on a CPU of the LLC it came from cuts RPC
      // throughput by about a factor of 10.  Seems to be much better
      // to resume the client directly.
      //
      // cp_->pool_.make_callback(cb, RecordRef{chunk, b});
      cb(RecordRef{chunk, b});
    }
    else if (msg.body.mtype() == CALL) {
      rpc_msg res;
      if (RpcService *s = srvmap_.lookup(msg, &res)) {
        auto awaitable = s->serve(msg, {b.subspan(hdrbytes), bufend});
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
