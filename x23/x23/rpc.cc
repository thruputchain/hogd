
#include <x23/rpc.h>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <netinet/in.h>
#include <optional>
#ifdef __linux__
#include <linux/tcp.h>
#endif
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

namespace {

void
sample_tcp_state(BorrowedFd fd, RpcReplyFrameTiming &row) noexcept
{
#ifdef __linux__
  tcp_info info{};
  socklen_t len = sizeof(info);
  if (getsockopt(fd.get(), IPPROTO_TCP, TCP_INFO, &info, &len) != 0 ||
      len > sizeof(info))
    return;
#define X23_RPC_TCP_FIELD(index, name)                                         \
  if (len >= offsetof(tcp_info, name) + sizeof(info.name)) {                   \
    row.tcp_valid |= uint64_t{1} << index;                                     \
    row.tcp[index] = info.name;                                                \
  }
  X23_RPC_TCP_FIELD(0, tcpi_rtt);
  X23_RPC_TCP_FIELD(1, tcpi_rttvar);
  X23_RPC_TCP_FIELD(2, tcpi_unacked);
  X23_RPC_TCP_FIELD(3, tcpi_snd_cwnd);
  X23_RPC_TCP_FIELD(4, tcpi_total_retrans);
  X23_RPC_TCP_FIELD(5, tcpi_notsent_bytes);
  X23_RPC_TCP_FIELD(6, tcpi_rwnd_limited);
  X23_RPC_TCP_FIELD(7, tcpi_sndbuf_limited);
#undef X23_RPC_TCP_FIELD
#else
  (void)fd;
  (void)row;
#endif
}

} // namespace

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

  auto cs = cstate_.take();
  auto oldmap = std::exchange(cs->xidmap_, {});
  if (!cs->frames_.empty()) {
    cs->frames_.clear();
    watching_frames_.store(false, std::memory_order_release);
  }
  cs.reset();
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
RpcStreamImpl::gen_call_xid(Awaitable<Buf> &a, const RpcQueueTiming *trace)
{
  auto cs = cstate_.take();
  if (ss_.stop_requested())
    throw XdrException("RPC call on failed file descriptor");
  for (;;) {
    std::uint32_t xid = ++cs->lastxid_;
    if (auto r = cs->xidmap_.emplace(xid, a); r.second) [[likely]] {
      if (trace && trace->report_frame) [[unlikely]] {
        static std::atomic_uint64_t next_connection{1};
        if (!cs->frame_connection_)
          cs->frame_connection_ =
              next_connection.fetch_add(1, std::memory_order_relaxed);
        if (cs->frames_.size() < 64) {
          try {
            cs->frames_.emplace(
                xid,
                PendingFrame{.timing = {.block = trace->block,
                                        .transaction = trace->transaction,
                                        .phase = trace->phase,
                                        .connection = cs->frame_connection_,
                                        .xid = xid,
                                        .outstanding = cs->xidmap_.size(),
                                        .registered = rpc_queue_now(),
                                        .framed = 0},
                             .report = trace->report_frame});
            watching_frames_.store(true, std::memory_order_release);
          } catch (const std::bad_alloc &) {
            // Missing samples are reported as incomplete by the reducer.
          }
        }
      }
      return xid;
    }
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
  while (!ss_.stop_requested()) {
    if (Errno err = co_await read_record(&b)) {
      fail();
      co_return;
    }
    const auto framed =
        watching_frames_.load(std::memory_order_acquire) ? rpc_queue_now() : 0;
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
      std::optional<PendingFrame> observation;
      if (framed) [[unlikely]] {
        if (auto sample = cs->frames_.find(msg.xid);
            sample != cs->frames_.end()) {
          observation.emplace(sample->second).timing.framed = framed;
          cs->frames_.erase(sample);
          if (cs->frames_.empty())
            watching_frames_.store(false, std::memory_order_release);
        }
      }
      cs.reset();
      if (observation) [[unlikely]] {
        sample_tcp_state(fd_, observation->timing);
        observation->report(observation->timing);
      }
      // Re-enqueing this on a CPU of the LLC it came from cuts RPC
      // throughput by about a factor of 10.  Seems to be much better
      // to resume the client directly.
      //
      // cp_->pool_.make_callback(cb, std::move(b));
      cb(std::move(b));
      if constexpr (kMinBuf != 0U)
        b.reserve(kMinBuf);
    }
    else if (msg.body.mtype() == CALL) {
      rpc_msg res;
      if (RpcService *s = srvmap_.lookup(msg, &res)) {
        std::optional<RpcQueueTiming> queue_timing;
        auto awaitable = [&] {
          const auto *probe =
              rpc_queue_server_probe.load(std::memory_order_acquire);
          const auto &call = msg.body.cbody();
          if (probe && call.prog == probe->program &&
              call.vers == probe->version &&
              (call.proc == probe->push || call.proc == probe->commit))
              [[unlikely]] {
            queue_timing.emplace().received = rpc_queue_now();
            auto *previous =
                std::exchange(rpc_queue_server_timing, &*queue_timing);
            Defer restore([&] { rpc_queue_server_timing = previous; });
            return s->serve(msg, {b.subspan(hdrbytes), b});
          }
          return s->serve(msg, {b.subspan(hdrbytes), b});
        }();
        // Only spawn another coroutine if result not already available
        decltype(auto) awaiter = detail::get_awaiter(awaitable);
        if (awaiter.await_ready())
          cp_->dwrite(fd_, awaiter.await_resume(),
                      queue_timing && queue_timing->report ? &*queue_timing
                                                           : nullptr);
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
