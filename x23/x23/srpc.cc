
#include <print>

#include "x23/fileio.h"
#include "x23/srpc.h"

namespace x23 {

SRpcFd::SRpcFd(UniqueFd fd)
  : max_message_size_(default_max_message_size), fd_(std::move(fd))
{}

SRpcFd::~SRpcFd()
{
  fail();
}

void
SRpcFd::fail()
{
  ss_.request_stop();
  shutdown(fd_.get(), SHUT_RDWR);
  write_ready();
}

void
SRpcFd::read_worker()
{
  VecQ<std::uint8_t> rdbuf;
  while (!ss_.stop_requested()) {
    size_t rdsz = kReadSize;
    if (rdbuf.size() > 4) {
      auto msglen =
          std::min<size_t>(max_message_size_,
                           getbe<std::uint32_t>(rdbuf.data()) & 0x7fffffff);
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

    while (rdbuf.size() >= 4) {
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

      // XXX - work in progress
    }

  }
}

void
SRpcFd::write_worker()
{
  using enum msg_type;
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
