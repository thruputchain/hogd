#include "hogd/hog_exec/interface/rpc_from_coord.h"

#include <array>
#include <cassert>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#ifdef __linux__
#include <sched.h>
#endif
#include <utility>
#include <vector>

#include "exec_result.hh"
#include "exec_sched.hh"
#include "hogd/base/log.h"
#include "hogd/base/os.h"
#include "hogd/base/perthread_arc.h"
#include "hogd/base/types.h"
#include "hogd/hog_exec/common/status.h"
#include "hogd/hog_exec/components/execution_mode.h"
#include "hogd/hog_exec/interface/executor_context.h"
#include "hogd/hog_exec/runtime/item/db_connection_map.h"
#include "rpc_types.hh"
#include "transaction.hh"
#include "x23/async.h"

namespace hogd {

namespace {

using ValidationClock = std::chrono::steady_clock;
struct ValidationTimes {
  std::array<int64_t, 5> phases;
  ValidationDbResumeSample db;
};
using ValidationKey = std::pair<uint16_t, BlockID>;

struct ValidationBlockSamples {
  std::vector<ValidationTimes> complete;
  ValidationClock::time_point first;
  ValidationClock::time_point last;
  size_t pending = 0;
  size_t incomplete = 0;
  size_t omitted = 0;
  bool closed = false;
};

std::mutex validation_samples_mutex;
std::map<ValidationKey, ValidationBlockSamples> validation_samples;
constexpr size_t kValidationSamplePeriod = 32;
constexpr size_t kMaxValidationBlocks = 128;
constexpr size_t kMaxValidationSamplesPerBlock = 4096;

struct ValidationSample {
  ValidationKey key;
  ValidationClock::time_point entry = ValidationClock::now();
  ValidationClock::time_point context_ready{};
  ValidationClock::time_point worker_resume{};
  ValidationClock::time_point batch_done{};
  ValidationClock::time_point released{};
  ValidationDbResumeSample db;

  ~ValidationSample()
  {
    // A finish racing a failed or otherwise unfinished request reports its
    // pending sample and removes the row. Never recreate a flushed row.
    std::lock_guard lock(validation_samples_mutex);
    auto it = validation_samples.find(key);
    if (it == validation_samples.end() || it->second.closed)
      return;
    auto &samples = it->second;
    --samples.pending;
    if (released == ValidationClock::time_point{}) {
      ++samples.incomplete;
      return;
    }
    auto us = [](auto start, auto end) {
      return std::chrono::duration_cast<std::chrono::microseconds>(end - start)
          .count();
    };
    samples.complete.push_back({{
                                    us(entry, released),
                                    us(entry, context_ready),
                                    us(context_ready, worker_resume),
                                    us(worker_resume, batch_done),
                                    us(batch_done, released),
                                },
                                db});
    samples.first = std::min(samples.first, entry);
    samples.last = std::max(samples.last, released);
  }
};

std::unique_ptr<ValidationSample>
start_validation_sample(ValidationKey key)
{
  static thread_local size_t sampling_sequence = 0;
  if (++sampling_sequence % kValidationSamplePeriod != 1)
    return nullptr;
  std::lock_guard lock(validation_samples_mutex);
  auto it = validation_samples.find(key);
  if (it == validation_samples.end()) {
    if (validation_samples.size() == kMaxValidationBlocks)
      return nullptr;
    it = validation_samples.try_emplace(key).first;
    it->second.first = ValidationClock::time_point::max();
  }
  auto &samples = it->second;
  if (samples.closed)
    return nullptr;
  if (samples.complete.size() + samples.incomplete + samples.pending ==
      kMaxValidationSamplesPerBlock) {
    ++samples.omitted;
    return nullptr;
  }
  // Reserve outside the sample destructor, which runs during coroutine cleanup.
  if (samples.complete.empty() && samples.complete.capacity() == 0)
    samples.complete.reserve(kMaxValidationSamplesPerBlock);
  auto sample = std::make_unique<ValidationSample>(key);
  ++samples.pending;
  return sample;
}

void
write_validation_samples(ValidationKey key, bool caller_hint)
{
  ValidationBlockSamples samples;
  {
    std::lock_guard lock(validation_samples_mutex);
    auto it = validation_samples.find(key);
    if (it == validation_samples.end() || it->second.closed)
      return;
    samples = std::move(it->second);
    it->second.closed = true;
  }
  static std::mutex output_mutex;
  std::lock_guard output_lock(output_mutex);
  std::ofstream out(std::getenv("HOGD_EXEC_VALIDATION_TIMING"), std::ios::app);
  if (!out) {
    warn("Cannot open executor validation timing output");
    return;
  }
  const auto n = samples.complete.size();
  // Each line is one bounded handler sample. The loop retains v1 microseconds;
  // the nested phases are nanoseconds.
  std::ofstream db_out(std::string(std::getenv("HOGD_EXEC_VALIDATION_TIMING")) +
                           ".db.tsv",
                       std::ios::app);
  if (db_out) {
    for (const auto &row : samples.complete) {
      db_out << "executor_validation_db_v1\t" << key.first << '\t' << key.second
             << '\t' << (caller_hint ? "caller" : "reader") << '\t'
             << row.phases[3] << '\t' << row.db.root_ns << '\t'
             << row.db.commit_total_ns << '\t' << row.db.event_launch_ns;
      for (const auto &rpc : {row.db.push, row.db.commit}) {
        db_out << '\t' << rpc.await_ns << '\t' << rpc.hop_ns << '\t'
               << rpc.replies << '\t' << rpc.same_llc;
      }
      db_out << '\n';
    }
    db_out.flush();
  }
  if (!db_out)
    warn("Cannot write executor DB resume timing output");
  const auto envelope_us =
      n == 0 ? 0
             : std::chrono::duration_cast<std::chrono::microseconds>(
                   samples.last - samples.first)
                   .count();
  const std::array phases{"handler", "lookup_and_id", "dispatch",
                          "transaction_loop", "release"};
  for (size_t phase = 0; phase < phases.size(); ++phase) {
    std::ranges::sort(samples.complete, {},
                      [phase](auto const &row) { return row.phases[phase]; });
    out << "executor_validation_v1\t" << key.first << '\t' << key.second << '\t'
        << kValidationSamplePeriod << '\t' << n << '\t' << samples.incomplete
        << '\t' << samples.pending << '\t' << samples.omitted << '\t'
        << envelope_us << '\t' << phases[phase] << '\t'
        << (n == 0 ? 0 : samples.complete[(n - 1) / 2].phases[phase]) << '\t'
        << (n == 0 ? 0 : samples.complete[(n * 9 + 9) / 10 - 1].phases[phase])
        << '\t' << (n == 0 ? 0 : samples.complete.back().phases[phase]) << '\n';
  }
  out.flush();
  if (!out)
    warn("Cannot write executor validation timing output");
}

} // namespace

int
ExecutorMempoolRpc::validation_db_connection_experiment(uint16_t bound_port)
{
  const char *value = std::getenv("HOGD_VALIDATION_DB_CONNECTIONS");
  if (!value)
    return 0;
  const auto matches = [](const char *name, std::string_view expected) {
    const char *setting = std::getenv(name);
    return setting && setting == expected;
  };
  if (!matches("HOGD_CI_CONTAINER", "1") || !matches("HOGD_CI_OFFLINE", "1") ||
      !matches("HOGD_VALIDATION_DB_RESUME_HINT", "reader") ||
      !(matches("HOG_HOST", "hog5") || matches("HOG_HOST", "hog9")) ||
      !(bound_port == 8040 || bound_port == 8041))
    throw std::runtime_error(
        "DB connection diagnostic outside isolated executor roles");
#ifdef __linux__
  cpu_set_t actual, expected;
  CPU_ZERO(&expected);
  const int first_cpu = bound_port == 8040 ? 0 : 32;
  for (int cpu = first_cpu; cpu < first_cpu + 32; ++cpu) {
    CPU_SET(cpu, &expected);
    CPU_SET(cpu + 64, &expected);
  }
  if (sched_getaffinity(0, sizeof(actual), &actual) != 0 ||
      !CPU_EQUAL(&actual, &expected) || available_parallelism() != 64)
    throw std::runtime_error(
        "DB connection diagnostic requires the exact executor CPU slots");
#else
  throw std::runtime_error("DB connection diagnostic requires Linux");
#endif
  int requested;
  if (std::string_view(value) == "32" &&
      matches("GIT_BRANCH", "codex/nightly-two-cohort-validation-batch64-db32"))
    requested = 32;
  else if (std::string_view(value) == "64" &&
           matches("GIT_BRANCH",
                   "codex/nightly-two-cohort-validation-batch64-db64"))
    requested = 64;
  else
    throw std::runtime_error("unrecognized DB connection diagnostic arm");
  // Reject an unstaffed target before attempting coordinator registration.
  (void)db_connection_experiment_map(requested);
  return requested;
}

void
ExecutorMempoolRpc::enable_validation_timing(uint16_t bound_port)
{
  if (const char *path = std::getenv("HOGD_EXEC_VALIDATION_TIMING");
      path != nullptr && *path != '\0') {
    validation_timing_port_ = bound_port;
    const auto matches = [](const char *name, std::string_view expected) {
      const char *value = std::getenv(name);
      return value && value == expected;
    };
    preserve_validation_db_hint_ =
        (bound_port == 8040 || bound_port == 8041) &&
        matches("GIT_BRANCH",
                "codex/nightly-two-cohort-validation-batch64-caller-hint") &&
        matches("HOGD_CI_CONTAINER", "1") && matches("HOGD_CI_OFFLINE", "1") &&
        (matches("HOG_HOST", "hog5") || matches("HOG_HOST", "hog9")) &&
        matches("HOGD_VALIDATION_DB_RESUME_HINT", "caller");
  }
}

x23::Async<SystemStatus<PerThreadARC<ExecutorContext> *>>
ExecutorMempoolRpc::try_get_context(BlockID block_id, ExecutionMode mode)
{
  if (!initialize_check()) [[unlikely]] {
    warn("ExecutorMempoolRpc: accessing context before initialization");
    co_return nullptr;
  }

  auto *ctx = co_await executor_states_->get_executor_context(block_id);
  if (!ctx) {
    co_return nullptr;
  }
  if (ctx->get().mode() != mode) {
    warn("Expected executor context for block {} to be in {} mode, but found "
         "in {} mode",
         block_id, mode, ctx->get().mode());
    if (auto rel = co_await release_context(block_id, ctx); !rel.has_value())
        [[unlikely]] {
      co_return rel.error();
    }
    co_return nullptr;
  }

  co_return ctx;
}

x23::Async<ExecProposeResult>
ExecutorMempoolRpc::executorsched_propose_transaction(
    BlockID block_id, SignedTransaction signed_tx)
{
  auto ctx_status =
      co_await try_get_context(block_id, ExecutionMode::kProposing);
  if (!ctx_status.has_value()) [[unlikely]] {
    co_return ExecProposeResult(ExecResult::ExecResultUnrecoverable);
  }
  auto *ctx = *ctx_status;
  if (!ctx) {
    co_return ExecProposeResult(ExecResult::ExecResultSystemError);
  }

  auto r = co_await ctx->get().propose_transaction(std::move(signed_tx),
                                                   allocate_tx_ids(1));
  if (auto rel = co_await release_context(block_id, ctx); !rel.has_value())
      [[unlikely]] {
    co_return ExecProposeResult(ExecResult::ExecResultUnrecoverable);
  }
  co_return r;
}

x23::Async<ExecResult>
ExecutorMempoolRpc::executorsched_validate_transaction(
    BlockID block_id, ProposedTransaction proposed_tx)
{
  auto ctx_status =
      co_await try_get_context(block_id, ExecutionMode::kValidating);
  if (!ctx_status.has_value()) [[unlikely]] {
    co_return ExecResult::ExecResultUnrecoverable;
  }
  auto *ctx = *ctx_status;
  if (!ctx) {
    co_return ExecResult::ExecResultSystemError;
  }

  auto r = co_await ctx->get().validate_transaction(std::move(proposed_tx),
                                                    allocate_tx_ids(1));
  if (auto rel = co_await release_context(block_id, ctx); !rel.has_value())
      [[unlikely]] {
    co_return ExecResult::ExecResultUnrecoverable;
  }
  co_return r;
}

x23::Async<ExecProposeBatchResult>
ExecutorMempoolRpc::executorsched_propose_batch(
    BlockID block_id, SignedTransactionBatch signed_txs)
{
  auto ctx_status =
      co_await try_get_context(block_id, ExecutionMode::kProposing);
  if (!ctx_status.has_value()) [[unlikely]] {
    co_return ExecProposeBatchResult(signed_txs.size(),
                                     ExecProposeResult(
                                         ExecResult::ExecResultUnrecoverable));
  }
  auto *ctx = *ctx_status;
  if (!ctx) {
    co_return ExecProposeBatchResult(signed_txs.size(),
                                     ExecProposeResult(
                                         ExecResult::ExecResultSystemError));
  }

  CHECK(signed_txs.size() <= std::numeric_limits<uint32_t>::max());
  // Preserve the error shape after transferring ownership into execution.
  const auto batch_size = signed_txs.size();
  auto first_tx_id = allocate_tx_ids(batch_size);
  auto r = co_await ctx->get().propose_transaction_batch(std::move(signed_txs),
                                                         first_tx_id);
  if (auto rel = co_await release_context(block_id, ctx); !rel.has_value())
      [[unlikely]] {
    co_return ExecProposeBatchResult(batch_size,
                                     ExecProposeResult(
                                         ExecResult::ExecResultUnrecoverable));
  }
  co_return r;
}

x23::Async<ExecValidateBatchResult>
ExecutorMempoolRpc::executorsched_validate_batch(
    BlockID block_id, ProposedTransactionBatch proposed_txs)
{
  std::unique_ptr<ValidationSample> sample;
  if (validation_timing_port_ != 0) [[unlikely]] {
    sample = start_validation_sample({validation_timing_port_, block_id});
  }
  auto ctx_status =
      co_await try_get_context(block_id, ExecutionMode::kValidating);
  if (!ctx_status.has_value()) [[unlikely]] {
    co_return ExecValidateBatchResult(proposed_txs.size(),
                                      ExecResult::ExecResultUnrecoverable);
  }
  auto *ctx = *ctx_status;
  if (!ctx) {
    co_return ExecValidateBatchResult(proposed_txs.size(),
                                      ExecResult::ExecResultSystemError);
  }

  CHECK(proposed_txs.size() <= std::numeric_limits<uint32_t>::max());
  const auto batch_size = proposed_txs.size();
  const auto first_tx_id = allocate_tx_ids(batch_size);
  if (sample)
    sample->context_ready = ValidationClock::now();
  auto r = co_await ctx->get().validate_transaction_batch(
      std::move(proposed_txs), first_tx_id,
      sample ? &sample->worker_resume : nullptr, preserve_validation_db_hint_,
      sample ? &sample->db : nullptr);
  if (sample)
    sample->batch_done = ValidationClock::now();
  if (auto rel = co_await release_context(block_id, ctx); !rel.has_value())
      [[unlikely]] {
    co_return ExecValidateBatchResult(batch_size,
                                      ExecResult::ExecResultUnrecoverable);
  }
  if (sample)
    sample->released = ValidationClock::now();
  co_return r;
}

x23::Async<ExecResult>
ExecutorMempoolRpc::executorsched_open_proposal_block(
    BlockID id, BlockExecutionContext block_execution)
{
  if (!initialize_check()) [[unlikely]] {
    co_return ExecResult::ExecResultSystemError;
  }
  co_return co_await executor_states_->open_proposal(id, block_execution);
}

x23::Async<ExecResult>
ExecutorMempoolRpc::executorsched_open_validation_block(
    BlockID id, BlockExecutionContext block_execution)
{
  if (!initialize_check()) [[unlikely]] {
    co_return ExecResult::ExecResultSystemError;
  }
  co_return co_await executor_states_->open_validation(id, block_execution);
}

x23::Async<ExecResult>
ExecutorMempoolRpc::executorsched_finish_block(BlockID id)
{
  if (!initialize_check()) [[unlikely]] {
    co_return ExecResult::ExecResultSystemError;
  }
  auto result = co_await executor_states_->remove_block(id);
  if (validation_timing_port_ != 0) [[unlikely]] {
    write_validation_samples({validation_timing_port_, id},
                             preserve_validation_db_hint_);
  }
  co_return result;
}

TransactionID
ExecutorMempoolRpc::allocate_tx_ids(uint32_t count)
{
  auto &allocator = *tx_id_allocator_;
  if (allocator.base_pool_ == nullptr) [[unlikely]] {
    allocator.base_pool_ = &base_alloc_pool_;
  }
  uint64_t base = allocator.allocate(count);
  CHECK(base + count <= uint64_t{1} << 48); // another executor's id block
  return TransactionID{base + (uint64_t{self_executor_id_} << 48)};
}

x23::Async<void>
ExecutorMempoolRpc::executorsched_shutdown()
{
  shut_down();
  co_return;
}

x23::MaybeAsync<SystemStatus<void>>
ExecutorMempoolRpc::release_context(BlockID block_id,
                                    PerThreadARC<ExecutorContext> *ctx)
{
  if (ctx->release_reference()) {
    // We dropped the last reference, so the deferred gc runs here. Its drain
    // can surface a late event-server error from a straggler callback; return
    // it so the caller can fold it into the in-flight transaction's result.
    return executor_states_->gc_executor_context(block_id);
  }
  return SystemStatus<void>{};
}

} // namespace hogd
