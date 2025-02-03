#include "hogd/hogexec/interface/executor_state.h"

#include <cassert>
#include <mutex>

namespace hogd {

bool
ExecutorState::try_acquire_run_lease(ExecutorMode expected_state)
{
  std::lock_guard lock(mtx_);
  assert(expected_state == ExecutorMode::kProposing ||
         expected_state == ExecutorMode::kValidating);

  if (current_state_ != expected_state) {
    return false;
  }

  ++running_transaction_count_;
  return true;
}

void
ExecutorState::release_run_lease()
{
  std::lock_guard lock(mtx_);

  assert(running_transaction_count_ > 0);
  --running_transaction_count_;

  if (running_transaction_count_ == 0 &&
      current_state_ == ExecutorMode::kFinishing) {
    current_state_ = ExecutorMode::kDone;
    cv_.notify_all();
  }
}

void
ExecutorState::await_finish()
{
  std::unique_lock lk(mtx_);
  assert(current_state_ != ExecutorMode::kDone);
  assert(current_state_ != ExecutorMode::kFinishing);

  current_state_ = ExecutorMode::kFinishing;
  if (running_transaction_count_ == 0) {
    current_state_ = ExecutorMode::kDone;
    return;
  }
  cv_.wait(lk, [this] { return current_state_ == ExecutorMode::kDone; });
}

} // namespace hogd
