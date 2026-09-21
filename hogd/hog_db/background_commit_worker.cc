#include "background_commit_worker.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <mutex>
#include <span>
#include <stop_token>
#include <thread>
#include <utility>
#include <vector>

#include "auth_rpc.hh"
#include "hogd/base/log.h"
#include "hogd/base/perthread.h"
#include "hogd/hog_db/auth_change_subscription_manager.h"
#include "sched_db.hh"
#include "types.hh"
#include "x23/async.h"

namespace hogd {

BackgroundCommitWorker::BackgroundCommitWorker(
    AuthChangeSubscriptionManager &auth_change_subscription_manager,
    std::shared_ptr<sched_db_client_t> sched_db_client,
    size_t max_deltas_per_chunk)
  : auth_change_subscription_manager_(auth_change_subscription_manager),
    sched_db_client_(std::move(sched_db_client)),
    max_deltas_per_chunk_(max_deltas_per_chunk)
{
  CHECK_GT(max_deltas_per_chunk_, 0UL);
  background_worker_ =
      std::jthread([this](std::stop_token stoken) { commit_worker(stoken); });
}

namespace {
// Sends a committed block's balance deltas to the scheduler as a run of
// chunks. Every chunk carries its index and the run's length, so the mempool
// accepts the run in any order and completes the block once all of it is in;
// a block with no nonzero delta still sends one empty chunk. This sends the
// chunks one at a time and awaits each, which also keeps the contract that
// the next block's run starts only after every chunk of this one is
// acknowledged; a sender may fan a run out over several connections instead.
void
dispatch_balance_deltas(
    BlockHeight block_height, PerThread<std::vector<Delta>> balance_deltas,
    size_t max_deltas_per_chunk,
    BackgroundCommitWorker::sched_db_client_t &sched_db_client)
{
  std::vector<Delta> deltas;
  // Release ul before calling the rpc client
  {
    auto ul = balance_deltas.lock_threads();
    balance_deltas.for_each(ul, [&](const std::vector<Delta> &thread_deltas) {
      deltas.insert(deltas.end(), thread_deltas.begin(), thread_deltas.end());
    });
  }

  const std::span<const Delta> all(deltas);
  const size_t chunk_count =
      std::max<size_t>(1, (all.size() + max_deltas_per_chunk - 1) /
                              max_deltas_per_chunk);
  for (size_t index = 0; index < chunk_count; ++index) {
    const size_t begin = index * max_deltas_per_chunk;
    const size_t count = std::min(max_deltas_per_chunk, all.size() - begin);
    HogsTransferredChunk chunk{.block = block_height,
                               .chunk_index = static_cast<std::uint32_t>(index),
                               .chunk_count =
                                   static_cast<std::uint32_t>(chunk_count),
                               .deltas = {}};
    chunk.deltas.append_range(all.subspan(begin, count));
    sched_db_client.scheddb_hogs_transferred_chunk(std::move(chunk))();
  }
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
  // This runs on a std::jthread, where an escaping throw is std::terminate
  // with no message. The scheduler callback is a post-commit side effect the
  // mempool gates the next block on, so once it fails the chain stalls either
  // way; fail stop with the height and the cause instead (issue #2332). The
  // noexcept wrappers keep a throw from panic()'s own formatting inside the
  // fail stop.
  try {
    auth_change_subscription_manager_.detached_publish_auth_changes(
        job.block_height_, std::move(job.key_change_batch_));
    if (sched_db_client_) {
      dispatch_balance_deltas(job.block_height_, std::move(job.balance_deltas_),
                              max_deltas_per_chunk_, *sched_db_client_);
    }
  } catch (const std::exception &e) {
    [&]() noexcept {
      panic("background commit of block {} failed; its scheduler callback is "
            "lost: {}",
            job.block_height_, e.what());
    }();
  } catch (...) {
    [&]() noexcept {
      panic("background commit of block {} failed; its scheduler callback is "
            "lost",
            job.block_height_);
    }();
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
