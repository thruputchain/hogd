// invoke_tx.h - handles transactions for contract invocations

#pragma once

#include <cassert>
#include <cstddef>
#include <optional>
#include <span>
#include <unordered_map>
#include <utility>
#include <vector>

#include "contract.hh"
#include "db_conflict.hh"
#include "hog_exec_constants.hh"
#include "hogd/base/log.h"
#include "hogd/base/perthread_arc.h"
#include "hogd/base/span.h"
#include "hogd/base/types.h"
#include "hogd/crypto/crypto.h"
#include "hogd/hog_db/block_tx.h"
#include "hogd/hog_db/hog_types.h"
#include "hogd/hog_db/item_change.h"
#include "hogd/hog_db/status.h"
#include "retrocausal_queue.hh"
#include "rpc_types.hh"
#include "storage_item.hh"
#include "transaction.hh"
#include "types.hh"

namespace hogd {

class RPCContext;

// BlockTXRef owns an InvokeTX's counted reference to a PerThreadARC<BlockTX>.
// The reference is released when the BlockTXRef is destroyed -- i.e. only
// after ~InvokeTX() has finished the destructor-time cleanup that
// dereferences the block -- so the BlockTX can never be freed while an
// InvokeTX that uses it is still alive (#2959). If the released reference is
// the last one on a shutdown block and a gc context was provided, the block
// is garbage collected via RPCContext::remove_shutdown_block_tx().
class BlockTXRef {
public:
  explicit BlockTXRef(PerThreadARC<BlockTX> &block) : arc_(&block) {}
  BlockTXRef(PerThreadARC<BlockTX> &block, RPCContext &gc_ctx, BlockTXID bid)
    : arc_(&block), gc_ctx_(&gc_ctx), bid_(bid)
  {}

  BlockTXRef(const BlockTXRef &) = delete;
  BlockTXRef &operator=(const BlockTXRef &) = delete;
  BlockTXRef(BlockTXRef &&other) noexcept
    : arc_(other.arc_), gc_ctx_(other.gc_ctx_), bid_(other.bid_),
      active_(std::exchange(other.active_, false))
  {}
  BlockTXRef &operator=(BlockTXRef &&) = delete;

  // Releases the reference and, on the last release of a shutdown block,
  // garbage collects the block through the gc context.
  ~BlockTXRef();

  BlockTX &get() const { return arc_->get(); }

private:
  PerThreadARC<BlockTX> *arc_;
  RPCContext *gc_ctx_ = nullptr;
  BlockTXID bid_ = 0;
  bool active_ = true;
};

// InvokeTX represents a transaction for a root contract invocation and its
// child invocations. Rolling back nested transactions is supported through
// savepoints. The caller must provide synchronization if needed.
class InvokeTX {
public:
  // The InvokeTX takes ownership of a counted reference to [block] that the
  // caller has already acquired; ~InvokeTX() releases it. RPCContext uses
  // the second overload so that the last release of a shutdown block
  // garbage collects it.
  InvokeTX(PerThreadARC<BlockTX> &block) : block_(block) {}
  InvokeTX(PerThreadARC<BlockTX> &block, RPCContext &gc_ctx, BlockTXID bid)
    : block_(block, gc_ctx, bid)
  {}
  ~InvokeTX() { abort(); }

  InvokeTX(InvokeTX &&other) noexcept = default;

  // commit commits pending changes to the BlockTX.
  HogDbStatus<void> commit();
  // abort rolls back pending changes to the BlockTX.
  void abort();

  // create_savepoint creates a savepoint of the current transaction state. It
  // returns a depth that can be used to restore to this state later.
  int create_savepoint();
  // rollback_to_savepoint restores the transaction state to a savepoint.
  void rollback_to_savepoint(int depth);
  // release_savepoint removes a savepoint and frees its associated resources.
  // If other savepoints were made after this savepoint, they will be removed
  // as well.
  void release_savepoint(int depth);

  // get_savepoint_head returns the depth of the most recent savepoint.
  unsigned int get_savepoint_head() const { return tx_depth_; }

  //
  // All of the read_* methods below read the current value of an item in just
  // this transaction relative to the snapshot state. They will return
  // kTypeMistmatch if the stored type is different from the requested type or
  // kNotFound if the item does not exist.
  //

  HogDbStatus<int128_t> read_nnint128(const ItemKey &key);
  HogDbStatus<OwningSpan<const std::byte>> read_bytes(const ItemKey &key);

  //
  // All of the request_* methods below are different operations that can be
  // performed during the first phase of two-phase commit. The request could
  // fail if there was a conflict (kConflict) or if the input was invalid
  // (e.g., kOverflow).
  //

  // request_nnint128 requests to perform set_add(base, delta).
  HogDbStatus<void> request_nnint128(const ItemKey &key, int128_t base,
                                     int128_t delta, StorageDeltaFlagSet flags);

  // request_nnint128 requests to perform set_add(SNAPSHOT.base, delta).
  HogDbStatus<void> request_nnint128(const ItemKey &key, int128_t delta,
                                     StorageDeltaFlagSet flags);

  // request_bytes requests to insert buf.
  HogDbStatus<void> request_bytes(const ItemKey &key,
                                  std::span<const std::byte> buf,
                                  StorageDeltaFlagSet flags);

  // request_deletion requests to delete the key.
  HogDbStatus<void> request_deletion(const ItemKey &key,
                                     StorageDeltaFlagSet flags);

  // request_deploy_contract requests to deploy a contract.
  // Note that this does NOT create the contract, or update a refcount on that
  // contract.
  HogDbStatus<void> request_deploy_contract(Address const &addr,
                                            const DeployedContractHash &hash);

  // request_new_contract requests to create a new contract.
  // Does NOT update the refcount on the contract.
  HogDbStatus<void> request_new_contract(Contract contract);

  // request_auth_data requests to change the auth data for an address.
  HogDbStatus<void>
  request_auth_data(Address const &addr,
                    const AuthDataChangeArgs &auth_data_change,
                    StorageDeltaFlagSet flags);
  // add uniquifier to replay cache if it is not already in the cache,
  // otherwise fail.
  HogDbStatus<void> request_consume_uniquifier(Address const &addr,
                                               Uniquifier uniquifier,
                                               StorageDeltaFlagSet flags);

  // request_retrocausal_queue requests to apply a retrocausal queue delta.
  // The delta is moved into the resulting ItemChange; callers that still need
  // their delta afterwards must pass a copy.
  HogDbStatus<void> request_retrocausal_queue(const ItemKey &key,
                                              RetrocausalQueueDelta &&delta,
                                              StorageDeltaFlagSet flags);

  void assert_is_valid() const noexcept;

  // Execution mode of the block this transaction runs in.
  BlockExecutionMode execution_mode() const { return block_.get().mode(); }

  // One failed balance-lock acquisition: the raw address whose balance
  // could not be reconciled and the conflict reason.
  struct BalanceConflict {
    ContractAddress addr;
    DbConflictCode reason;
  };

  // Synchronizes balance locks for all addresses touched at the current
  // savepoint depth.
  //
  // On failure with [all_conflicts] null: aborts at the first failing
  // address, reporting it via [first_conflicting_addr] if non-null.
  // With [all_conflicts] non-null: keeps evaluating the remaining
  // addresses best-effort, appending every conflict/overflow failure, and
  // returns the first error; any non-conflict failure (e.g. stale block)
  // aborts immediately without being appended. The conflict reason, if
  // any, is recorded in last_conflict_reason().
  HogDbStatus<void>
  sync_balance_locks(ContractAddress *first_conflicting_addr = nullptr,
                     std::vector<BalanceConflict> *all_conflicts = nullptr);

  // Reason for the most recent kConflict returned by a request_* method or
  // sync_balance_locks (#2759). Meaningful only immediately after a request
  // that returned kConflict; a stale value may remain after other failures.
  std::optional<DbConflictCode> last_conflict_reason() const
  {
    return last_conflict_reason_;
  }

  // Maps a failed request's error code to the DbConflictCode reported to
  // the executor: kOverflow maps directly; kConflict reads the recorded
  // reason. CHECK rather than assert: silently defaulting a missing reason
  // would misreport the conflict type in release builds.
  DbConflictCode conflict_reason_for(HogDbCode code) const;

  // Number of keys retained in the transaction-local change and balance-delta
  // maps. Tests use these to assert that a failed subtransaction leaves no
  // entries behind once it is rolled back (#3090); nothing in production
  // reads them.
  std::size_t change_entry_count_for_testing() const { return changes_.size(); }
  std::size_t balance_entry_count_for_testing() const
  {
    return balance_deltas_.size();
  }

private:
  struct ItemVersion {
    ItemChange change;
    unsigned int tx_depth;
    int128_t locked_storage_cost{0};

    ItemVersion(ItemChange &&change, unsigned int tx_depth,
                int128_t locked_storage_cost = 0)
      : change(std::move(change)), tx_depth(tx_depth),
        locked_storage_cost(locked_storage_cost)
    {}

    // Disable copy and allow move.
    // This is not strictly necessary, but eliminates the possibility
    // of accidental copies that caused the *(almost) bug in PR #1196
    ItemVersion(ItemVersion &&) noexcept = default;
    ItemVersion &operator=(ItemVersion &&) noexcept = default;
  };

  struct StorageBalanceVersion {
    // The amount of storage that was manually transferred (by explicit contract
    // call) in or out of this address.
    int128_t manual_transfer_amount;
    // total locked amount at this depth,
    // for all ops on this address.
    int128_t total_locked_storage_amount;
    unsigned int tx_depth;
  };

  // block_ is the first data member so it is destroyed last: destructor-time
  // cleanup (~InvokeTX() -> abort()) dereferences the block, so the counted
  // reference must outlive every other part of teardown (#2959).
  BlockTXRef block_;
  unsigned int tx_depth_{0};
  std::optional<DbConflictCode> last_conflict_reason_;
  std::unordered_map<ItemKey, std::vector<ItemVersion>> changes_;
  // Whenever you request a balance change (from a user action),
  // combine it with the latest balance delta for that address.
  // Whenever you update a balance change from a storage operation,
  // also update the balance_deltas_ for that address.
  std::unordered_map<Address, std::vector<StorageBalanceVersion>>
      balance_deltas_;

  // read_change returns the most recent version of a change, or nullptr if
  // there is no change.
  const ItemChange *read_change(const ItemKey &key) const;

  // request_change tries to apply a change to the transaction.
  HogDbStatus<void> request_change(const ItemKey &key, ItemChange &&change);

  void request_balance_change(Address const &addr, int128_t delta);

  // lock_storage locks the storage for the given address.
  // cost must be nonnegative.
  void lock_storage(Address const &addr, int128_t cost);

  HogDbStatus<void> update_balance_lock(Address const &addr);

  // get_storage_balance_change returns the storage balance change for the given
  // address.
  HogDbStatus<Balance> get_storage_balance_change(Address const &addr) const;
};

inline int
InvokeTX::create_savepoint()
{
  CHECK(tx_depth_ < kMaxSavepointDepth);
  return ++tx_depth_;
}

inline const ItemChange *
InvokeTX::read_change(const ItemKey &key) const
{
  auto it = changes_.find(key);
  if (it == changes_.end())
    return nullptr;

  const std::vector<ItemVersion> &v = it->second;
  return &v.back().change;
}

inline HogDbStatus<void>
InvokeTX::request_bytes(const ItemKey &key, std::span<const std::byte> buf,
                        StorageDeltaFlagSet flags)
{
  auto change = ItemChange(Bytes{buf});
  if (flags & std::to_underlying(StorageDeltaFlags::ExclusiveLock)) {
    change.set_permanent_exclusive();
  }
  return request_change(key, std::move(change));
}

inline HogDbStatus<void>
InvokeTX::request_deletion(const ItemKey &key, StorageDeltaFlagSet flags)
{
  auto change = ItemChange(Deletion{});
  if (flags & std::to_underlying(StorageDeltaFlags::ExclusiveLock)) {
    change.set_permanent_exclusive();
  }
  return request_change(key, std::move(change));
}

inline HogDbStatus<void>
InvokeTX::request_deploy_contract(Address const &addr,
                                  const DeployedContractHash &hash)
{
  return request_change(make_address_metadata_item_key(addr),
                        DeployedContractData(hash));
}

} // namespace hogd
