#pragma once

#include <condition_variable>
#include <mutex>

#include "hogd/base/types.h"

namespace hogd {

enum class ExecutorMode : int32_t {
  kNone = 0,
  kProposing = 1,
  kValidating = 2,
  kFinishing = 3,
};

// State of the executor.  Tracks number of running threads
// and state of current block (proposing vs. validating).
class ExecutorState {
public:
  // Should be called by worker threads.
  // If try_acquire returns true,
  // the thread is responsible for later calling
  // release when done with its work.
  bool try_acquire_run_lease(ExecutorMode expected_state);
  void release_run_lease();

  // Should only be called from an RPC from the coordinator.
  // Cannot be called concurrently with each other (this would be an error
  // on the part of the coordinator).
  void await_finish();
  void start_proposing();
  void start_validating();

  ExecutorMode get_state()
  {
    std::lock_guard lock(mtx);
    return current_state_;
  }

private:
  std::mutex mtx;
  std::condition_variable cv;

  ExecutorMode current_state_ = ExecutorMode::kNone;

  uint32_t running_transaction_count_ = 0;
};

} // namespace hogd
