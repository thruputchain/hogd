#pragma once

#include <atomic>
#include <limits>
#include <memory>
#include <random>
#include <unordered_map>
#include <utility>
#include <vector>

#include "exec_coord.hh"
#include "hogd/base/types.h"
#include "hogd/base/worker_pool.h"
#include "hogd/hogexec/interface/executor_state_manager.h"
#include "hogd/hogexec/runtime/database_proxy.h"
#include "rpc_types.hh"
#include "sched_coord.hh"
#include "transaction.hh"
#include "types.hh"
#include "x23/async.h"
#include "x23/rpc.h"
#include "x23/vec.h"

namespace hogd {

// The mempool accepts transactions from the coordinator along with block
// nomination and validation requests.
class Mempool {
public:
  using rpc_service = SchedulerCoord1;

  using db_client_t =
      DbCoord1::client<x23::RpcStream<x23::RpcLogger>::ClientRef>;

  using exec_client_t =
      ExecutorCoord1::client<x23::RpcStream<x23::RpcLogger>::ClientRef>;

  Mempool(db_client_t &&db_client, exec_client_t &&exec_client)
    : db_client_(std::move(db_client)), exec_client_(std::move(exec_client))
  {}

  // Disable copy and move.
  Mempool(Mempool const &) = delete;
  Mempool &operator=(Mempool const &) = delete;
  Mempool(Mempool &&) = delete;
  Mempool &operator=(Mempool &&) = delete;

  static x23::Async<void> schedulercoord_null() { co_return; }

  // Submit an unvetted tx.
  x23::Async<void>
  schedulercoord_submit_unvetted(SignedTransactionBatch const &batch);

  // Submit a vetted tx.
  x23::Async<void>
  schedulercoord_submit_vetted(ValidatorID validator_id,
                               SignedTransactionBatch const &batch);

  // Have the mempool put together a block of transactions and nominate the
  // block. [db_tip] specifies the last committed block.
  x23::Async<SchedulerNominationResult>
  schedulercoord_nominate(DbTip const &db_tip);

  // Have the mempool validate a block. [nomination] holds the block to
  // validate. [db_tip] specifies the last committed block.
  x23::Async<SchedulerValidationResult>
  schedulercoord_validate(DbTip const &db_tip, Nomination const &nomination);

  // It is an error to shut down the mempool while there are transactions that
  // are still running.
  x23::Async<void> executorcoord_shutdown();

  void await_shutdown_request() { shutdown_flag_.wait(false); }

private:
  // The maximum number of txs in a block.
  static constexpr size_t kMaxBlockSize = 128;
  // To compute the block hash, we insert tx hashes into a Merkle Trie. We make
  // insertion faster with multiple threads. Using a lot of threads could cause
  // worse performance if each thread has few hashes to insert. Thus, to
  // calculate a reasonable number of threads to use, we ensure that each thread
  // has at least [kMinHashesPerThread] hashes to insert.
  static constexpr size_t kMinHashesPerThread = 16'384;

  // TODO(jhumphri): We are using this function to generate unique block IDs.
  // This will need to be reworked if/when we have multiple threads processing
  // blocks.
  static uint64_t get_ns_since_epoch()
  {
    std::chrono::time_point<std::chrono::system_clock> now =
        std::chrono::system_clock::now();
    std::chrono::nanoseconds ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            now.time_since_epoch());
    return ns.count();
  }

  bool find_vetted_txs(const Nomination &nomination,
                       x23::Vec<ProposedTransaction> &ret);

  bool create_snapshot(BlockID prev_block_id, BlockID block_id,
                       BlockExecutionMode);
  void abandon_snapshot(BlockID block_id);
  void close_snapshot(BlockID block_id);
  void finish_block(BlockID block_id);

  SHA256Hash get_block_hash(const x23::Vec<SignedTransactionHash> &txs);

  bool propose_tx(BlockID block_id, const SignedTransaction &tx,
                  x23::XdrVec<NominatedTransaction> &transaction_nominations);
  bool
  propose_batch(BlockID block_id, const x23::Vec<SignedTransaction> &txs,
                x23::XdrVec<NominatedTransaction> &transaction_nominations);

  bool open_proposal_block(BlockID block_id);

  bool validate_tx(BlockID block_id, const ProposedTransaction &tx);
  bool validate_batch(BlockID block_id,
                      const x23::Vec<ProposedTransaction> &txs);

  bool open_validation_block(BlockID block_id);

  db_client_t db_client_;
  exec_client_t exec_client_;

  std::unordered_map<SHA256Hash, SignedTransaction> vetted_txs_;

  std::atomic<bool> shutdown_flag_ = false;
};

static_assert(is_SchedulerCoord1<Mempool *, x23::Async>);

} // namespace hogd
