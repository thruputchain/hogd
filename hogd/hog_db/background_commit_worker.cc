#include "background_commit_worker.h"

#include <memory>
#include <mutex>
#include <stop_token>
#include <thread>
#include <utility>
#include <vector>

#include "auth_rpc.hh"
#include "hogd/base/perthread.h"
#include "hogd/hog_db/auth_change_subscription_manager.h"
#include "sched_db.hh"
#include "types.hh"
#include "x23/async.h"

namespace hogd {

BackgroundCommitWorker::BackgroundCommitWorker(
    AuthChangeSubscriptionManager &auth_change_subscription_manager,
    std::shared_ptr<sched_db_client_t> sched_db_client)
  : auth_change_subscription_manager_(auth_change_subscription_manager),
    sched_db_client_(std::move(sched_db_client))

{
  background_worker_ =
      std::jthread([this](std::stop_token stoken) { commit_worker(stoken); });
}

namespace {
x23::Async<void>
dispatch_balance_deltas(
    BlockHeight block_height, PerThread<std::vector<Delta>> balance_deltas,
    BackgroundCommitWorker::sched_db_client_t &sched_db_client)
{
  HogsTransferred transferred{.block = block_height, .deltas = {}};
  // Release ul before calling the rpc client
  {
    auto ul = balance_deltas.lock_threads();
    balance_deltas.for_each(ul, [&](const std::vector<Delta> &deltas) {
      transferred.deltas.append_range(deltas);
    });
  }
  return sched_db_client.scheddb_hogs_transferred(std::move(transferred));
}
} // namespace

void
BackgroundCommitWorker::submit_job(BlockHeight block_height,
                                   PerThread<KeyChangeBatch> key_changes,
                                   PerThread<std::vector<Delta>> balance_deltas)
{
  std::unique_lock lock(mutex_);
  jobs_.emplace(Job{.key_change_batch_ = std::move(key_changes),
                    .balance_deltas_ = std::move(balance_deltas),
                    .block_height_ = block_height});
  cv_.notify_one();
}

void
BackgroundCommitWorker::commit_job(Job &job)
{
  auth_change_subscription_manager_.detached_publish_auth_changes(
      job.block_height_, std::move(job.key_change_batch_));
  if (sched_db_client_) {
    dispatch_balance_deltas(job.block_height_, std::move(job.balance_deltas_),
                            *sched_db_client_)();
  }
}

void
BackgroundCommitWorker::commit_worker(std::stop_token &stoken)
{
  while (!stoken.stop_requested()) {
    std::unique_lock lock(mutex_);
    cv_.wait(lock, stoken, [this]() { return !jobs_.empty(); });
    if (stoken.stop_requested()) {
      return;
    }
    Job job = std::move(jobs_.front());
    jobs_.pop();
    lock.unlock();
    commit_job(job);
  }
}

} // namespace hogd
