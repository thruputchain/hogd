
#include <print>

#include "x23/sockio.h"
#include "x23/serialize.h"
#include "x23/srpc.h"
#include "x23/vec.h"

namespace x23 {

namespace {
using enum msg_type;
} // namespace

SRpcFd::SRpcFd(UniqueFd fd)
  : max_message_size_(default_max_message_size), fd_(std::move(fd))
{
  set_nodelay(fd);
  set_nonblock(fd, false);
}

SRpcFd::~SRpcFd()
{
  fail();
  bool running = false;
  {
    auto q = write_queue_.take();
    if (write_thread_.joinable())
      running = true;
  }
  if (running) {
    read_thread_.join();
    write_thread_.join();
  }
}

void
SRpcFd::fail()
{
  if (ss_.request_stop()) {
    shutdown(fd_.get(), SHUT_RDWR);
    write_ready();
    decltype(pending_calls_)::value_type pcs;
    pcs.swap(*pending_calls_.take());
    for (auto &pc : pcs)
      pc.second(nullptr, {});
  }
}

void
SRpcFd::go()
{
  auto q = write_queue_.take();
  assert(!write_thread_.joinable());
  write_thread_ = std::thread{&SRpcFd::write_worker, this};
  read_thread_ = std::thread{&SRpcFd::read_worker, this};
}

void
SRpcFd::write_call(Buf record, CallCB cb)
{
  auto q = write_queue_.take();
  if (ss_.stop_requested()) {
    q.reset();
    cb(nullptr, {});
    return;
  }
  q->emplace_back(std::move(record), std::move(cb));
}

void
SRpcFd::read_worker()
{
  VecQ<std::uint8_t> rdbuf;
  while (!ss_.stop_requested()) {
    size_t rdsz = kReadSize;
    if (rdbuf.size() > 4) {
      std::uint32_t msglen = getbe<std::uint32_t>(rdbuf.data()) & 0x7fffffff;
      msglen = std::min<std::uint32_t>(msglen, max_message_size_);
      if (msglen > rdbuf.size())
        rdsz = std::max(rdsz, msglen - rdbuf.size());
    }
    rdsz = std::max(rdsz, rdbuf.capacity());
    auto res = read(fd_, &rdbuf, rdsz);
    if (!res || !*res) {
      if (!res)
        std::println(stderr, "SRpcFd::read_worker fd {}: {}", fd_.get(),
                     res.error());
      fail();
      return;
    }

    while (rdbuf.size() >= 4 && !ss_.stop_requested()) {
      std::uint32_t msglen = getbe<std::uint32_t>(rdbuf.data()) ^ 0x80000000;

      if (msglen > 0x7fffffff) {
        std::println(stderr,
                     "SRpcFd::read_worker fd {}: partial messages unsupported",
                     fd_.get());
        fail();
        return;
      }
      if (msglen > max_message_size_) {
        std::println(stderr,
                     "SRpcFd::read_worker fd {}: messages size {} too big",
                     fd_.get(), msglen);
        fail();
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
SRpcFd::input(BufView bytes)
{
  rpc_msg msg;
  size_t n;
  try {
    n = deserialize_from(&msg, bytes);
  } catch (const XdrException &e) {
    std::println(stderr, "SRpcFd::input fd {}: {}", fd_.get(), e.what());
    fail();
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
      write_ready_.notify_one();
    return;
  }

  auto pcs = pending_calls_.take();
  if (auto pc = pcs->find(msg.xid); pc != pcs->end()) {
    auto cb{std::move(pc->second)};
    pcs->erase(pc);
    pcs.reset();
    cb(&msg, bytes.subspan(n));
  }
  else
    std::println(stderr, "SRpcFd::input fd {}: unknown Xid in reply",
                 fd_.get());
}

void
SRpcFd::write_worker()
{
  BufVec wrbuf;
  for (;;) {
    auto ptr = write_queue_.take();
    for (;;) {
      if (ss_.stop_requested())
        return;
      if (!ptr->empty())
        break;
      ptr.wait(write_ready_);
    }

    Vec<WriteItem> q;
    q.swap(*ptr);
    ptr.reset();

    auto pcs = pending_calls_.take();
    for (auto &it : q)
      if (it.replycb_) {
        assert(it.data_.size() >= 32 &&
               getbe<u_int32_t>(it.data_.data() + 4) == uint32_t(CALL) &&
               getbe<uint32_t>(it.data_.data() + 8) == 2);
        do {
          putbe(it.data_.data(), ++lastxid_);
        } while (!pcs->try_emplace(lastxid_, std::move(it.replycb_)).second);
      }
    pcs.reset();

    wrbuf.move_append_range(
        q | std::views::transform(std::mem_fn(&WriteItem::data_)));
    for (;;) {
      auto res = wrbuf.write(fd_.get());
      if (!res) {
        std::println(stderr, "SRpcFd::write_worker fd {}: {}", fd_.get(),
                     res.error());
        fail();
        return;
      }
      if (res && !*res)
        break;
    }
  }
}

} // namespace x23
