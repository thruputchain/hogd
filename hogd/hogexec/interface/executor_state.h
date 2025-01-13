#pragma once

#include <cassert>
#include <condition_variable>
#include <mutex>

#include "hogd/base/types.h"

namespace hogd {

enum class ExecutorMode : int32_t {
  kProposing = 0,
  kValidating = 1,
  kFinishing = 2,
  kDone = 3,
};

// State of the executor.  Tracks number of running threads
// and state of current block (proposing vs. validating).
class ExecutorState {
public:
  ExecutorState(ExecutorMode initial_mode) : current_state_(initial_mode)
  {
    assert(initial_mode == ExecutorMode::kProposing ||
           initial_mode == ExecutorMode::kValidating);
  }

  // Should be called by worker threads.
  // If try_acquire returns true,
  // the thread is responsible for later calling
  // release when done with its work.
  bool try_acquire_run_lease(ExecutorMode expected_state);
  void release_run_lease();

  // Should only be called from an RPC from the coordinator.
  // Coordinator should only call this method once.
  void await_finish();

  ExecutorMode get_state()
  {
    std::lock_guard lock(mtx);
    return current_state_;
  }

  ~ExecutorState() { assert(running_transaction_count_ == 0); }

private:
  std::mutex mtx;
  std::condition_variable cv;

  ExecutorMode current_state_;

  uint32_t running_transaction_count_ = 0;
};

} // namespace hogd
