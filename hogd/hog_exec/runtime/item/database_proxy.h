#pragma once

#include <cassert>
#include <optional>
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
#include "x23/async.h"
#include "x23/rpc.h"

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

  // Every call below is a coroutine that suspends until hog_db answers, so the
  // thread is free to run other transactions meanwhile.  A key argument still
  // live after the suspension is taken BY VALUE, so it is copied into the
  // coroutine frame rather than left dangling; one consumed before the first
  // suspension, only to build the request, is taken by reference.  The two
  // batch arguments below are too large to copy and are documented as
  // caller-owned instead.
  //
  // Each one resumes on a pool thread rather than on the connection's reader.
  // x23 resumes a reply's waiter inline, which is right when the waiter only
  // posts a semaphore, but here the waiter is the rest of a transaction: it
  // would run to its next database read on the reader, and that reader cannot
  // parse another reply while it does.  So every body below hops with
  // default_threadpool().bg() before touching the result, leaving the reader
  // one frame deep for as long as a re-enqueue takes.  Without the hop the
  // damage scales with how much work sits between two database reads, which
  // is why it showed up first as multiexec_interface throughput falling off
  // as batch_size rose.
  x23::Async<SystemStatus<std::optional<WireStorageItem>>>
  get_snapshot_item(BlockID block_id, AddressAndKey addrkey);

  x23::Async<SystemStatus<std::optional<Contract>>>
  get_contract(BlockID block_id, SHA256Hash const &contract_hash);

  x23::Async<SystemStatus<std::optional<DeployedContractHash>>>
  get_deployment(BlockID block_id, ContractAddress const &addr);

  x23::Async<SystemStatus<std::optional<RetrocausalQueueBlock>>>
  get_next_retrocausal_queue_block(BlockID block_id, AddressAndKey addrkey,
                                   uint64_t search_start_number);

  x23::Async<SystemStatus<std::optional<QueueEntry>>>
  get_concurrent_retrocausal_queue_entry(BlockID block_id,
                                         AddressAndKey addrkey, uint32_t index);

  // On kRPCStorageWriteFailed (returns false), if [conflict_out] is
  // non-null it receives the conflict diagnostics hog_db built at the level
  // it selected from the block's execution mode.
  //
  // [delta_batch] and [conflict_out] are borrowed across the suspension: this
  // is a coroutine, so a reference parameter is NOT copied into the frame and
  // the caller must keep the referent alive until the call completes.  The
  // batch is far too large to copy per push, which is why it stays a
  // reference; PerTransactionDatabaseProxy owns both for the whole call.
  x23::Async<SystemStatus<bool>>
  try_push_new_subtransaction(BlockID block_id, TransactionID tx_id,
                              StorageDeltaBatch const &delta_batch,
                              DbConflictResult *conflict_out = nullptr);

  // [new_contracts] is borrowed across the suspension; see above.
  x23::Async<SystemStatus<void>>
  commit_transaction(BlockID block_id, TransactionID tx_id,
                     BorrowedContractBatch const &new_contracts);

  x23::Async<SystemStatus<void>> abort_transaction(BlockID block_id,
                                                   TransactionID tx_id);

  x23::Async<SystemStatus<void>>
  pop_subtransactions(BlockID block_id, TransactionID tx_id, uint32_t count);

  x23::Async<SystemStatus<DbTip>> get_db_tip(BlockID block_id);

  x23::Async<SystemStatus<ViewingSnapshot>> open_viewing_snapshot();
  x23::Async<SystemStatus<void>> close_viewing_snapshot(BlockID block_id);

private:
  // The async multiclient: a DB read suspends the calling coroutine instead of
  // parking its thread, which is what lets contract execution share the x23
  // ThreadPool rather than needing a DispatchPool of its own.  Still a
  // multiclient (one connection per LLC): a synchronous-bodied async server
  // serializes requests per connection, so a single connection would serialize
  // every DB read in the process.
  using base_client_t = x23::RpcMulticlient<x23::RpcLogger>::ClientRef;
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

  // Coroutine: the staged batch stays owned by this object across the
  // suspension, and is cleared only once hog_db has answered.
  //
  // Defined out of line: GCC emits an inline coroutine's ramp even where it is
  // unused, so a header definition would make every includer's link need
  // DatabaseProxy.
  x23::Async<SystemStatus<bool>> try_push_current_subtransaction();

  x23::Async<SystemStatus<void>> pop_subtransactions(uint32_t count)
  {
    return db_proxy_.pop_subtransactions(block_id_, tx_id_, count);
  }

  x23::Async<SystemStatus<void>> commit_transaction()
  {
    static const BorrowedContractBatch::hash_set_t kEmptyHashes;
    static const BorrowedContractBatch::contract_map_t kEmptyContracts;
    static const BorrowedContractBatch kEmptyBatch(kEmptyHashes,
                                                   kEmptyContracts);
    return db_proxy_.commit_transaction(block_id_, tx_id_, kEmptyBatch);
  }

  // [new_contracts] is borrowed for the duration of the call; the caller owns
  // it (see DatabaseProxy::commit_transaction).
  x23::Async<SystemStatus<void>>
  commit_transaction(BorrowedContractBatch const &new_contracts)
  {
    return db_proxy_.commit_transaction(block_id_, tx_id_, new_contracts);
  }

  x23::Async<SystemStatus<void>> abort_transaction()
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
