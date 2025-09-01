
#include <print>

#include <signal.h>

#include "x23/serialize.h"
#include "x23/sockio.h"
#include "x23/srpc.h"
#include "x23/vec.h"

namespace x23 {

namespace {
using enum msg_type;
} // namespace

SRpcFd::SRpcFd(UniqueFd fd)
  : max_message_size_(default_max_message_size), fd_(std::move(fd))
{
  set_nodelay(fd_);
  set_nonblock(fd_, false);
}

SRpcFd::~SRpcFd()
{
  stop();
  if (write_thread_.joinable()) {
    // The only reference keeping server-only SRpcFds alive will be in
    // write_worker, meaning SRpcFds is destoyed as write_worker
    // exits.  We can't join ourselves so just detach.
    if (std::this_thread::get_id() == write_thread_.get_id())
      write_thread_.detach();
    else
      write_thread_.join();
  }
}

SRpcFd::Xid
SRpcFd::genxid()
{
  auto cs = calls_.take();
  for (++cs->lastxid_; cs->cbmap_.contains(cs->lastxid_) || !cs->lastxid_;
       ++cs->lastxid_)
    ;
  return cs->lastxid_;
}

void
SRpcFd::stop() noexcept
{
  if (ss_.request_stop()) {
    shutdown(fd_.get(), SHUT_RDWR);
    write_ready();
    decltype(PendingCalls::cbmap_) pcs;
    pcs.swap(calls_->cbmap_);
    for (auto &pc : pcs)
      pc.second(nullptr, {});
  }
}

void
SRpcFd::go(const CpuSet *cpus)
{
  // If this fails, you have neither clients nor servers.
  assert(refcount() || !srvmap_.empty());

  if (ignore_sigpipe) {
    ignore_sigpipe = false;
    signal(SIGPIPE, SIG_IGN);
  }

  // We lock write_queue_ to guard against the unlikely possibility
  // that go() gets called concurrently.
  auto _lk = write_queue_.take();
  if (!write_thread_.joinable()) {
    // If there are clients, we rely on the clients to hold the
    // refcount.  If there are only servers, then we rely on selfref
    // to ensure the SRpcFd is deleted when the write thread exits.
    RefPtr<SRpcFd> selfref{nullptr};
    if (refcount() == 0)
      selfref = RefPtr{*this};
    read_thread_ = std::thread{&SRpcFd::read_worker, this, cpus};
    write_thread_ =
        std::thread{&SRpcFd::write_worker, this, cpus, std::move(selfref)};
  }
}

void
SRpcFd::write_call(Buf record, CallCB cb)
{
  auto q = write_queue_.take();
  // Important to check stop_requested while locking write_queue_, as
  // otherwise we could enqueue it after stop() flushed the queue.
  if (ss_.stop_requested()) {
    q.reset();
    cb(nullptr, {});
    return;
  }
  bool need_wake = q->empty();
  q->emplace_back(std::move(record), std::move(cb));
  if (need_wake)
    this->write_ready();
}

void
SRpcFd::read_worker(const CpuSet *cpus) noexcept
{
  if (cpus)
    cpus->set_affinity();
  VecQ<std::uint8_t> rdbuf;
  while (!ss_.stop_requested()) {
    // resize_and_overwrite requires one extra byte, so reading
    // exactly capacity would trigger a reallocation.
    size_t rdsz =
        std::max(rdbuf.capacity(), kReadSize + 1UZ) - 1 - rdbuf.size();
    if (rdbuf.size() >= 4) {
      // Might as well ask for full message if we know the size
      size_t msglen = getbe<uint32_t>(rdbuf.data()) & 0x7fffffff;
      rdsz = std::max(rdsz, 4UZ + msglen - rdbuf.size());
    }
    auto res = read(fd_, &rdbuf, rdsz);
    if (!res || !*res) {
      if (!res && res.error() != ECONNRESET)
        fail("read_worker: {}", res.error());
      else
        stop();
      return;
    }

    while (rdbuf.size() >= 4 && !ss_.stop_requested()) {
      size_t msglen = getbe<uint32_t>(rdbuf.data()) ^ 0x80000000;

      if (msglen > 0x7fffffff) {
        fail("read_worker: partial messages unsupported");
        return;
      }
      if (msglen > max_message_size_) {
        fail("read_worker: message size {} too big", msglen);
        return;
      }
      if (rdbuf.size() < 4 + msglen)
        break;

      input({rdbuf.data() + 4, msglen});
      rdbuf.pop_front(4 + msglen);
    }
  }
}

void
SRpcFd::input(BufView bytes) noexcept
{
  rpc_msg msg;
  size_t n;
  try {
    n = deserialize_from(&msg, bytes);
  } catch (const XdrException &e) {
    fail("input fd: {}", e.what());
    return;
  }

  if (msg.body.mtype() == CALL) {
    Buf resp;
    if (SRpcService *s = srvmap_.lookup(msg, &msg))
      resp = s->serve(msg, bytes.subspan(n));
    else
      resp = serialize(msg);
    auto q = write_queue_.take();
    bool need_wake = q->empty();
    q->emplace_back(std::move(resp), nullptr);
    if (need_wake)
      write_ready();
    return;
  }

  auto cs = calls_.take();
  if (auto pc = cs->cbmap_.find(msg.xid); pc != cs->cbmap_.end()) {
    auto cb{std::move(pc->second)};
    cs->cbmap_.erase(pc);
    cs.reset();
    cb(&msg, bytes.subspan(n));
  }
  else {
    cs.reset();
    std::println(stderr, "SRpcFd::input fd {}: unknown Xid {} in reply",
                 fd_.get(), msg.xid);
  }
}

void
SRpcFd::write_worker(const CpuSet *cpus, RefPtr<SRpcFd> _selfref) noexcept
{
  if (cpus)
    cpus->set_affinity();
  Defer _join([this] { read_thread_.join(); });
  BufVec wrbuf;
  for (;;) {
    Vec<WriteItem> q;
    for (;;) {
      write_ready_ = false;
      if (ss_.stop_requested())
        return;
      q.swap(*write_queue_.take());
      if (!q.empty())
        break;
      write_ready_.wait(false);
    }

    auto calls = calls_.take();
    for (auto &it : q)
      if (it.replycb_) {
        assert(it.data_.size() >= 36 &&
               getbe<uint32_t>(it.data_.data() + 8) == uint32_t(CALL) &&
               getbe<uint32_t>(it.data_.data() + 12) == 2);
        Xid xid = getbe<uint32_t>(it.data_.data() + 4);
        if (!xid)
          putbe(it.data_.data() + 4, xid = ++calls->lastxid_);
        else if (calls->lastxid_ - xid > 0x8000'0000U)
          calls->lastxid_ = xid;
        while (!calls->cbmap_.try_emplace(xid, std::move(it.replycb_)).second)
          putbe(it.data_.data() + 4, xid = ++calls->lastxid_);
      }
    calls.reset();

    wrbuf.move_append_range(
        q | std::views::transform(std::mem_fn(&WriteItem::data_)));
    for (;;) {
      auto res = wrbuf.write(fd_.get());
      if (!res) {
        if (res.error() == ECONNRESET || res.error() == EPIPE)
          stop();
        else
          fail("write_worker: {}", res.error());
        return;
      }
      if (!*res)
        break;
    }
  }
}

} // namespace x23
