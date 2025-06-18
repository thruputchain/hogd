
#include "x23/srpc.h"

namespace x23 {

SRpcFd::SRpcFd(UniqueFd fd)
  : fd_(std::move(fd))
{
}

SRpcFd::~SRpcFd()
{
  ss_.request_stop();
  shutdown(fd_.get(), SHUT_RDWR);
  {
    auto _ptr = write_queue_.take();
    write_ready_.notify_all();
  }
}

void
SRpcFd::read_worker()
{}

void
SRpcFd::write_worker()
{
  using enum msg_type;

  for (;;) {
    auto ptr = write_queue_.take();
    if (ss_.stop_requested())
      return;
    if (ptr->empty()) {
      ptr.wait(write_ready_);
      continue;
    }
    Vec<WriteItem> q;
    q.swap(*write_queue_.take());

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

    // 
  }
}

} // namespace x23
