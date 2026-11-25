
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
RecvBuf::make_room(size_t more)
{
  const size_t n = nbuffered();
  const bool roomy = chunk_ && space() >= std::max(more, kMinRead);
  if (chunk_ && chunk_->unique()) {
    // No reply points into the chunk, so the bytes below rptr() are
    // ours again.  Rewinding an empty buffer is free; a partial
    // record is only worth moving when the tail is too small.
    if (!n || !roomy) {
      if (n && rpos_)
        std::memmove(chunk_->data(), rptr(), n);
      rpos_ = 0;
      wpos_ = n;
    }
    if (space() >= more)
      return;
  }
  else if (roomy)
    // Replies still point into the chunk, so keep filling its tail.
    return;

  // Start a new chunk.  Replies that reference the old one keep it
  // alive.  (n is 0 when there is no chunk yet.)
  auto fresh = RecvChunk::make(std::max(n + more, kChunkSize));
  if (n)
    std::memcpy(fresh->data(), rptr(), n);
  chunk_ = std::move(fresh);
  rpos_ = 0;
  wpos_ = n;
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
    // A fragment is a 4-byte marker followed by len bytes.  Read
    // until the buffer holds a complete one; marker and len are not
    // used before then.
    const size_t have = in.nbuffered();
    std::uint32_t marker = 0;
    size_t len = 0;
    size_t need = 4;
    if (have >= 4) {
      marker = getbe<std::uint32_t>(in.rptr());
      len = marker & ~kLastFragment;
      if (frags.size() + len > max_message_size) {
        // NOLINTNEXTLINE(readability-static-accessed-through-instance)
        debug.err(
            "RpcStreamImpl: fd {} received record size {} exceeds maximum {}",
            get_fd(), frags.size() + len, max_message_size);
        fail();
        co_return;
      }
      need = 4 + len;
    }
    if (have < need) {
      in.make_room(need - have);
      auto res = co_await cp_->read_some(fd_, in.wptr(), in.space());
      if (!res) {
        // read_some reports a spurious wakeup instead of waiting again.
        if (res.error() == EAGAIN)
          continue;
        debug("RpcStreamImpl: read(fd {}): {}", get_fd(), res.error());
        fail();
        co_return;
      }
      if (*res == 0) {
        if (have || !frags.empty())
          // NOLINTNEXTLINE(readability-static-accessed-through-instance)
          debug.err("RpcStreamImpl: fd {} closed in the middle of a record",
                    get_fd());
        fail();
        co_return;
      }
      in.commit(*res);
      continue;
    }

    // A single-fragment record is parsed where it lies in the chunk.
    // Deserialization may read, but not use, bytes up to bufend.
    BufView b{in.rptr() + 4, len};
    const std::uint8_t *bufend = in.bufend();
    in.consume(4 + len);

    // Rare: collect a multi-fragment record in frags, then copy it
    // into a chunk of its own so that a reply can reference it.
    const bool last = (marker & kLastFragment) != 0U;
    RefPtr<RecvChunk> whole{nullptr};
    if (!last || !frags.empty()) [[unlikely]] {
      frags.append(b.begin(), b.end());
      if (!last)
        continue;
      whole = RecvChunk::make(frags.size());
      std::memcpy(whole->data(), frags.data(), frags.size());
      b = {whole->data(), frags.size()};
      bufend = b.data() + b.size();
      frags.clear();
    }

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
      // cp_->pool_.make_callback(cb, RecordRef{...});
      cb(RecordRef{whole ? whole : in.chunk(), b});
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
