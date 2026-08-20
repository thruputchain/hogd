#pragma once

#include <algorithm>
#include <cassert>
#include <optional>
#include <random>
#include <set>
#include <utility>
#include <variant>
#include <vector>

#include "contract.hh"
#include "db_conflict.hh"
#include "executor_to_db_rpc.hh"
#include "hogd/base/compiler.h"
#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "hogd/hog_exec/common/status.h"
#include "hogd/hog_exec/components/borrowed_contract_batch.h"
#include "retrocausal_queue.hh"
#include "rpc_types.hh"
#include "sched_db.hh"
#include "sched_exec.hh"
#include "storage_item.hh"
#include "types.hh"
#include "x23/srpc.h"

namespace hogd {

struct LocalMockDB {
  uint32_t read_port_number_;
};

struct ShardedDB {
  ShardedDB(IntroExecResult const &coord_info)
    : shards_(coord_info.shards.begin(), coord_info.shards.end())
  {}

  std::vector<ShardCard> shards_;
};

// Information identifying where and how to access the database.
// LocalMockDB redirects RPC calls to a server on localhost.
using DatabaseIdentifier = std::variant<LocalMockDB, ShardedDB>;

// Abstract away database sharding from the rest of the executor.
// Handles the RPC Error codes, changing them into restricted forms
// that the executor can handle (or abort on).
class DatabaseProxy {
public:
  DatabaseProxy(DatabaseIdentifier identifier);

  DatabaseProxy(DatabaseProxy const &) = delete;
  DatabaseProxy &operator=(DatabaseProxy const &) = delete;
  DatabaseProxy(DatabaseProxy &&) = delete;
  DatabaseProxy &operator=(DatabaseProxy &&) = delete;

  SystemStatus<std::optional<WireStorageItem>>
  get_snapshot_item(BlockID block_id, AddressAndKey const &addrkey);

  SystemStatus<std::optional<Contract>>
  get_contract(BlockID block_id, SHA256Hash const &contract_hash);

  SystemStatus<std::optional<DeployedContractHash>>
  get_deployment(BlockID block_id, ContractAddress const &addr);

  SystemStatus<std::optional<RetrocausalQueueBlock>>
  get_next_retrocausal_queue_block(BlockID block_id,
                                   AddressAndKey const &addrkey,
                                   uint64_t search_start_number);

  SystemStatus<std::optional<QueueEntry>>
  get_concurrent_retrocausal_queue_entry(BlockID block_id,
                                         AddressAndKey const &addrkey,
                                         uint32_t index);

  // On kRPCStorageWriteFailed (returns false), if [conflict_out] is
  // non-null it receives the conflict diagnostics hog_db built at the level
  // it selected from the block's execution mode.
  SystemStatus<bool>
  try_push_new_subtransaction(BlockID block_id, TransactionID tx_id,
                              StorageDeltaBatch const &delta_batch,
                              DbConflictResult *conflict_out = nullptr);

  SystemStatus<void>
  commit_transaction(BlockID block_id, TransactionID tx_id,
                     BorrowedContractBatch const &new_contracts);

  SystemStatus<void> abort_transaction(BlockID block_id, TransactionID tx_id);

  SystemStatus<void> pop_subtransactions(BlockID block_id, TransactionID tx_id,
                                         uint32_t count);

  SystemStatus<DbTip> get_db_tip(BlockID block_id);

  SystemStatus<ViewingSnapshot> open_viewing_snapshot();
  SystemStatus<void> close_viewing_snapshot(BlockID block_id);

private:
  using base_client_t = x23::SRpcMulticlient<x23::RpcLogger>::ClientRef;
  using client_t = DatabaseToExecutorInterfaceV1::client<base_client_t>;
  client_t client_;
};

// A DatabaseProxy that is scoped to a single transaction.
class PerTransactionDatabaseProxy {
public:
  PerTransactionDatabaseProxy(DatabaseProxy &db_proxy, BlockID block_id,
                              TransactionID tx_id)
    : db_proxy_(db_proxy), block_id_(block_id), tx_id_(tx_id)
  {}

  // A copy would fork the staged delta batch below, which used to be a shared
  // thread_local. Several signatures take this by value.
  PerTransactionDatabaseProxy(PerTransactionDatabaseProxy &&) = default;
  PerTransactionDatabaseProxy(const PerTransactionDatabaseProxy &) = delete;
  PerTransactionDatabaseProxy &
  operator=(const PerTransactionDatabaseProxy &) = delete;

  void reserve(size_t delta_count)
  {
    current_subtransaction_batch_.reserve(delta_count);
  }

  void add_delta(AddressAndKey addrkey, StorageDelta delta,
                 StorageDeltaFlagSet flags)
  {
    current_subtransaction_batch_.emplace_back(std::move(addrkey),
                                               std::move(delta), flags);
  }

  // Conflict diagnostics from the most recent failed push, at the level
  // the database selected from the block mode; the None arm if the last
  // push succeeded or the block is not a proposal.
  DbConflictResult const &last_push_conflict() const
  {
    return last_push_conflict_;
  }

  SystemStatus<bool> try_push_current_subtransaction()
  {
    if constexpr (is_debug_build()) {
      // Invariant: hog_exec sends at most one delta per key per subtransaction.
      std::set<AddressAndKey> seen_keys;
      for (auto const &d : current_subtransaction_batch_) {
        auto [_, inserted] = seen_keys.insert(d.addrkey);
        CHECK(inserted);
      }

      // Shuffle the batch of deltas before sending to the database because
      // the order of deltas in a batch is supposed to not affect the result
      // of the call.  Shuffling in tests helps catch bugs in the database
      // delta handling logic.
      std::shuffle(current_subtransaction_batch_.begin(),
                   current_subtransaction_batch_.end(),
                   std::mt19937{std::random_device{}()});
    }

    last_push_conflict_ =
        DbConflictResult(DbDiagnosticLevel::kDbDiagnosticNone);
    auto r =
        db_proxy_.try_push_new_subtransaction(block_id_, tx_id_,
                                              current_subtransaction_batch_,
                                              &last_push_conflict_);
    current_subtransaction_batch_.clear();
    return r;
  }

  SystemStatus<void> pop_subtransactions(uint32_t count)
  {
    return db_proxy_.pop_subtransactions(block_id_, tx_id_, count);
  }

  SystemStatus<void> commit_transaction()
  {
    static const BorrowedContractBatch::hash_set_t kEmptyHashes;
    static const BorrowedContractBatch::contract_map_t kEmptyContracts;
    static const BorrowedContractBatch kEmptyBatch(kEmptyHashes,
                                                   kEmptyContracts);
    return db_proxy_.commit_transaction(block_id_, tx_id_, kEmptyBatch);
  }

  SystemStatus<void>
  commit_transaction(BorrowedContractBatch const &new_contracts)
  {
    return db_proxy_.commit_transaction(block_id_, tx_id_, new_contracts);
  }

  SystemStatus<void> abort_transaction()
  {
    return db_proxy_.abort_transaction(block_id_, tx_id_);
  }

private:
  // Deltas staged for the next try_push_current_subtransaction().
  //
  // This used to be a `static thread_local`, which reuses one heap buffer for
  // every transaction a worker thread runs.  That is only sound while a
  // transaction owns its thread from start to finish, as it does on a
  // DispatchPool worker.  Syscalls can now suspend (see syscall_entry), so two
  // transactions interleave on one thread and the second would clear and
  // overwrite the first's staged deltas while it is parked in
  // try_push_new_subtransaction().  Per-proxy state is the only shape that
  // survives interleaving.
  //
  // The cost is that the buffer is reallocated per transaction rather than per
  // thread.  If that shows up, hand the buffer out of a thread-local free list
  // (cf. rv32::MemorySlot) rather than putting the batch itself back in TLS --
  // acquire and release both touch only the running thread's pool, so that
  // stays correct under interleaving.
  StorageDeltaBatch current_subtransaction_batch_;
  DatabaseProxy &db_proxy_;
  const BlockID block_id_;
  const TransactionID tx_id_;
  DbConflictResult last_push_conflict_{DbDiagnosticLevel::kDbDiagnosticNone};
};
} // namespace hogd
