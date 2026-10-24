#pragma once

#include <condition_variable>
#include <cstddef>
#include <memory>
#include <mutex>
#include <queue>
#include <stop_token>
#include <thread>
#include <vector>

#include "auth_rpc.hh"
#include "hogd/base/perthread.h"
#include "hogd/base/types.h"
#include "hogd/hog_db/auth_change_subscription_manager.h"
#include "sched_db.hh"
#include "types.hh"
#include "x23/rpc.h"
#include "x23/rpc_common.h"

namespace hogd {

// Publishes a committed block's side effects off the commit path: its auth
// changes to the auth subscribers and its native-token balance deltas to the
// scheduler.
class BackgroundCommitWorker {
public:
  using sched_db_client_t =
      SchedDb1::client<x23::RpcStream<x23::RpcLogger>::ClientRef>;

  // Deltas per NativeTokensTransferredChunk. A Delta encodes to 48 bytes, so a
  // full chunk is 12 MiB of deltas, far below the link's record cap; a run has
  // up to available_parallelism() chunks in flight at once.
  static constexpr size_t kMaxDeltasPerChunk = size_t{1} << 18;

  // [max_deltas_per_chunk] is a test seam; production uses the default.
  BackgroundCommitWorker(
      AuthChangeSubscriptionManager &auth_change_subscription_manager,
      std::shared_ptr<sched_db_client_t> sched_db_client,
      size_t max_deltas_per_chunk = kMaxDeltasPerChunk);

  void submit_job(BlockHeight block_height,
                  PerThread<KeyChangeBatch> key_changes,
                  PerThread<std::vector<Delta>> balance_deltas);

  ~BackgroundCommitWorker()
  {
    background_worker_.request_stop();
    cv_.notify_all();
  }

private:
  struct Job {
    PerThread<KeyChangeBatch> key_change_batch_;
    PerThread<std::vector<Delta>> balance_deltas_;
    BlockHeight block_height_;
  };

  void commit_worker(std::stop_token &stoken);
  void commit_job(Job &job);

  std::queue<Job> jobs_;

  AuthChangeSubscriptionManager &auth_change_subscription_manager_;
  std::shared_ptr<sched_db_client_t> sched_db_client_;
  size_t max_deltas_per_chunk_;

  std::mutex mutex_;
  std::condition_variable_any cv_;

  std::jthread background_worker_;
};
} // namespace hogd
