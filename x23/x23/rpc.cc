
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

bool
RpcStreamImpl::input(const RefPtr<RecvChunk> &chunk, BufView record,
                     const std::uint8_t *bufend)
{
  using enum msg_type;

  // Every rpc_msg starts with the xid followed by the message type.
  if (record.size() < 8) [[unlikely]] {
    debug("RpcStreamImpl: fd {} received {}-byte record", get_fd(),
          record.size());
    fail();
    return false;
  }

  if (getbe<std::uint32_t>(record.data() + 4) == std::uint32_t(REPLY)) {
    // We leave decoding the rest of the header to the client that is
    // waiting for this reply, so as to do it only once and off the
    // reader's critical path.
    const auto xid = getbe<std::uint32_t>(record.data());
    auto cs = cstate_.take();
    auto it = cs->xidmap_.find(xid);
    if (it == cs->xidmap_.end())
      return true;
    auto cb = std::move(it->second);
    cs->xidmap_.erase(it);
    cs.reset();
    // Re-enqueing this on a CPU of the LLC it came from cuts RPC
    // throughput by about a factor of 10.  Seems to be much better
    // to resume the client directly.
    //
    // cp_->pool_.make_callback(cb, RecordRef{chunk, record});
    cb(RecordRef{chunk, record});
    return true;
  }

  rpc_msg msg;
  size_t hdrbytes;
  try {
    hdrbytes = deserialize_from(&msg, BufViewEnd{record, bufend});
  } catch (XdrException &e) {
    debug("RpcStreamImpl deserialization: {}", e.what());
    fail();
    return false;
  }
  if (msg.body.mtype() != CALL) [[unlikely]] {
    debug("RpcStreamImpl: fd {} received bad message type", get_fd());
    fail();
    return false;
  }

  rpc_msg res;
  if (RpcService *s = srvmap_.lookup(msg, &res)) {
    // The arguments are only valid until we return, because the
    // reader will go on to reuse the chunk.
    auto awaitable = s->serve(msg, {record.subspan(hdrbytes), bufend});
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
  return true;
}

// Reads into a chunk and parses every complete record in place.
Detached
RpcStreamImpl::reader(SLOC_ARG0)
{
  RefPtr<RpcStreamImpl> _r = *this;
  RecvBuf in;
  // Earlier fragments of a multi-fragment record.  We never send
  // these ourselves, but ONC RPC allows them.
  Buf frags;

  while (!ss_.stop_requested()) {
    // Number of bytes we still need before we can make progress.
    size_t need;
    if (in.avail() < 4)
      need = 4 - in.avail();
    else {
      const auto marker = getbe<std::uint32_t>(in.rptr());
      const size_t len = marker & ~kLastFragment;
      if (frags.size() + len > max_message_size) {
        // NOLINTNEXTLINE(readability-static-accessed-through-instance)
        debug.err(
            "RpcStreamImpl: fd {} received record size {} exceeds maximum {}",
            get_fd(), frags.size() + len, max_message_size);
        break;
      }
      if (in.avail() >= 4 + len) {
        BufView body{in.rptr() + 4, len};
        in.consume(4 + len);
        if (!(marker & kLastFragment)) [[unlikely]]
          frags.append(body.begin(), body.end());
        else if (frags.empty()) [[likely]] {
          if (!input(in.chunk(), body, in.bufend()))
            co_return;
        }
        else {
          // Reassemble into a chunk of its own, so that a reply can
          // outlive the reader's buffer as usual.
          const size_t total = frags.size() + len;
          auto whole = RecvChunk::make(total);
          std::memcpy(whole->data(), frags.data(), frags.size());
          std::memcpy(whole->data() + frags.size(), body.data(), len);
          frags.clear();
          if (!input(whole, {whole->data(), total}, whole->data() + total))
            co_return;
        }
        continue;
      }
      need = 4 + len - in.avail();
    }

    in.make_room(need);
    auto res = co_await cp_->read_some(fd_, in.wptr(), in.space());
    if (!res) {
      if (res.error() == EAGAIN)
        continue;
      debug("RpcStreamImpl: read(fd {}): {}", get_fd(), res.error());
      break;
    }
    if (*res == 0) {
      if (in.avail() || !frags.empty())
        // NOLINTNEXTLINE(readability-static-accessed-through-instance)
        debug.err("RpcStreamImpl: fd {} closed in the middle of a record",
                  get_fd());
      break;
    }
    in.commit(*res);
  }
  fail();
}

} // namespace x23::detail
