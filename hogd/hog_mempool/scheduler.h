#pragma once

#include <array>
#include <atomic>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "db_sched.hh"
#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "hogd/container/synchronized_map.h"
#include "hogd/crypto/crypto.h"
#include "hogd/hog_mempool/block_transfers.h"
#include "hogd/hog_mempool/shared.h"
#include "hogd/xdr/signed_transaction_hash.h"
#include "sched_db.hh"
#include "sched_gateway.hh"
#include "transaction.hh"
#include "types.hh"
#include "x23/x23/async.h"
#include "x23/x23/corosync.h"
#include "x23/x23/vec.h"

namespace hogd {

class Scheduler {
public:
  static constexpr BlockHeight kTransactionOutcomeBlockLifetime = 50;
  static constexpr size_t kMaxTransactionOutcomes = 50'000'000;
  // The most native-token ages one DbSched1 record carries, requested or
  // written. A NativeTokenAge is 104 bytes on the wire, so a record stays near
  // 7 MiB, under x23's 16 MiB default record cap, whichever way it travels.
  static constexpr size_t kMaxNativeTokenAgesPerRecord = size_t{1} << 16;

  Scheduler(BlockHeight current_block,
            std::shared_ptr<db_sched_client_t> db_sched_client)
    : current_block_(current_block),
      db_sched_client_(std::move(db_sched_client))
  {}

  ~Scheduler() { wait_for_pending_detached_coroutines(); }

  // Which submission path a batch for [add_txs()] came from, which decides
  // what happens to a submission whose body is already cached under a
  // different signed encoding.
  enum class CachedVariant {
    // [submit_unvetted()]: keep the cached encoding and report the submission
    // admitted.
    kSubmitUnvetted,
    // [schedulercoord_submit_vetted()]: replace the cached encoding with the
    // submitted one. The coordinator supplies the encodings a proposer chose,
    // so the cache holds what the block under validation executes.
    kSubmitVetted,
  };

  x23::Async<void> shut_down()
  {
    wait_for_pending_detached_coroutines();
    co_return;
  }

  // Adds txs to the scheduler and returns one admission outcome per tx, in
  // input order. Adding the same tx multiple times is okay; a duplicate that
  // is queued or in a pending nomination is reported as admitted rather than
  // enqueued again.
  //
  // This method will cycle the groups for an account if doing so gives the
  // account an advantage in scheduling. This happens when (1) cycling increases
  // the average age of the [kNativeTokenAgeGroupToSchedule] group or (2) the
  // [kNativeTokenAgeGroupToSchedule] group does not have enough native tokens
  // to schedule the tx but cycling will add enough native tokens to this group
  // to schedule it.
  //
  // [signed_tx_hashes] holds each tx's [SignedTransactionHash], aligned with
  // [txs] and [tx_hashes]. [on_variant] decides between a submission and a
  // different signed encoding of its body that is already cached.
  x23::Async<SchedGatewaySubmitOutcomeBatch>
  add_txs(const x23::XdrVec<SignedTransaction> &txs,
          const x23::XdrVec<TransactionHash> &tx_hashes,
          const SignedTransactionHashBatch &signed_tx_hashes,
          CachedVariant on_variant);

  // [add_txs()] for callers that have not hashed the signed encodings, as an
  // unvetted submission.
  x23::Async<SchedGatewaySubmitOutcomeBatch>
  add_txs(const x23::XdrVec<SignedTransaction> &txs,
          const x23::XdrVec<TransactionHash> &tx_hashes);

  x23::Async<void> process_transfers(const BlockTransfers &transfer);

  // Chooses txs to schedule in the next block. [block_height] is the height of
  // the block that is being scheduled. [block_size] is the maximum number of
  // txs that will be included in the next block, though fewer txs than
  // [block_size] may be included.
  //
  // Appends each scheduled tx to [txs], its [TransactionHash] to [tx_hashes]
  // and the [SignedTransactionHash] of its cached encoding to
  // [signed_tx_hashes], keeping the three aligned.
  x23::Async<void> schedule(size_t num_txs,
                            x23::XdrVec<TransactionHash> &tx_hashes,
                            SignedTransactionHashBatch &signed_tx_hashes,
                            x23::XdrVec<SignedTransaction> &txs);

  void start_pending_nomination(BlockID block_id);
  x23::Async<void> commit_pending_nomination(BlockID block_id);
  x23::Async<void> abandon_pending_nomination(BlockID block_id);
  x23::Async<void> apply_committed_block_native_token_ages(
      const x23::XdrVec<SignedTransaction> &txs, BlockHeight block_height);

  std::shared_ptr<const SignedTransaction>
  find_tx(const TransactionHash &tx_hash)
  {
    auto [tx_value_ptr, _] = txs_by_body_.find(tx_hash);
    if (tx_value_ptr) {
      return std::shared_ptr<const SignedTransaction>(*tx_value_ptr,
                                                      &(*tx_value_ptr)->tx);
    }
    return {};
  }

  // The cached tx with this exact signed encoding, or null if its body is not
  // cached or is cached under a different encoding. Encodings that vetted
  // submissions displaced are still found. When the tx is found and
  // [tx_hash] is non-null, sets [*tx_hash] to the tx's [TransactionHash].
  std::shared_ptr<const SignedTransaction>
  find_signed_tx(const SignedTransactionHash &signed_tx_hash,
                 TransactionHash *tx_hash = nullptr)
  {
    auto [tx_value_ptr, _] = txs_.find(signed_tx_hash);
    if (tx_value_ptr) {
      if (tx_hash) {
        *tx_hash = (*tx_value_ptr)->tx_hash;
      }
      return std::shared_ptr<const SignedTransaction>(*tx_value_ptr,
                                                      &(*tx_value_ptr)->tx);
    }
    return {};
  }

  x23::Async<SchedulerTransactionOutcome>
  lookup_transaction_outcome(const TransactionHash &tx_hash);

  x23::Async<void> record_transaction_outcome(
      const TransactionHash &tx_hash, SchedulerTransactionStatus status,
      BlockHeight height,
      SchedulerTransactionErrorSource error_source =
          SchedulerTransactionErrorSource::SchedulerTransactionErrorNone,
      int32_t error_code = 0);

  // Records successful commits in bounded batches, sharing pruning and lock
  // acquisition across transactions. Existing committed outcomes keep their
  // original height, just as in [record_transaction_outcome].
  x23::Async<void> record_committed_transaction_outcomes(
      x23::span<const TransactionHash> tx_hashes, BlockHeight height);

  x23::Async<void> clear_transaction_outcome_if_not_already_committed(
      const TransactionHash &tx_hash);

  void get_txs_for_account(const ContractAddress &address,
                           TransactionHashBatch &tx_hashes,
                           SignedTransactionBatch &txs)
  {
    // The method is called by [MempoolCoordinatorRpc], which may try to find
    // txs associated with an account that the scheduler does not know about. In
    // that case, just return.
    auto [account_state, lock] = accounts_.find(address);
    if (!account_state) {
      return;
    }

    tx_hashes.reserve(account_state->txs.size());
    txs.reserve(account_state->txs.size());
    for (const auto &[tx_key, tx_value] : account_state->txs) {
      tx_hashes.push_back(tx_key.tx_hash);
      txs.push_back(tx_value->tx);
    }
  }

  void reenqueue_txs(const auto &begin, const auto &end)
  {
    size_t num_inserted = 0;
    for (auto it = begin; it != end; it++) {
      const TransactionHash &tx_hash = *it;
      std::shared_ptr<TxValue> tx_value;
      {
        const auto [tx_value_ptr, lock] = txs_by_body_.find(tx_hash);
        CHECK_PTR(tx_value_ptr);
        tx_value = *tx_value_ptr;
      }

      const SignedTransaction &tx = tx_value->tx;
      const ContractAddress &account = tx.tx.invocation.invoked_address;
      const TxKey key{
          .expiry_block_height = tx.tx.uniquifier.block_expiry_height,
          .tx_hash = tx_hash,
      };
      auto [account_state, account_lock] = get_account_state(account);

      // [schedule()] or the commit path may have removed the signed tx cache
      // entry since the lookup above. Revalidate signed tx while holding both
      // locks in account-then-cache order so a runqueue entry cannot outlive
      // its cache entry. Use the newly cached signed tx value because a
      // replacement signed tx may carry different signatures for the same
      // transaction hash.
      const auto [cached_value, txs_lock] = txs_by_body_.find(tx_hash);
      if (cached_value &&
          account_state->txs.insert({key, *cached_value}).second) {
        num_inserted++;
      }
    }
    size_.fetch_add(num_inserted, std::memory_order_release);
    MempoolMetrics::instance().scheduler_pending_tx_count.record(size());
    MempoolMetrics::instance().scheduler_tx_cache_size.record(tx_cache_size());
  }

  x23::Async<void>
  remove_from_scheduler(x23::span<const TransactionHash> hashes);

  // We are exposing the functions below for testing. They should not be called
  // directly by non-test code.

  std::pair<NativeTokenAge *, std::unique_lock<std::mutex>>
  get_cached_native_token_age(const ContractAddress &address);

  x23::Async<void> write_all_average_native_token_ages_to_db();

  // Bounds the native-token ages per DbSched1 record read or written;
  // production keeps the default.
  void
  set_max_native_token_ages_per_record(size_t max_native_token_ages_per_record)
  {
    CHECK_GT(max_native_token_ages_per_record, size_t{0});
    max_native_token_ages_per_record_ = max_native_token_ages_per_record;
  }

  struct ReadAverageNativeTokenAgesResult {
    std::unordered_set<ContractAddress, SipHash24Hasher> found;
    std::unordered_set<ContractAddress, SipHash24Hasher> missing;
  };
  x23::Async<ReadAverageNativeTokenAgesResult>
  read_average_native_token_ages(const x23::XdrVec<ContractAddress> &accounts);
  x23::Async<ReadAverageNativeTokenAgesResult>
  read_average_native_token_ages(const x23::XdrVec<SignedTransaction> &txs);
  x23::Async<ReadAverageNativeTokenAgesResult>
  read_average_native_token_ages(const BlockTransfers &transfers);

  // Moves the native tokens between groups as specified by the scheduling
  // protocol. Specifically:
  // 1. Native tokens in the [kNativeTokenAgeGroupAlreadyScheduled] group are
  // moved to the [kNativeTokenAgeGroupToSchedule] group.
  // 2. Then, native tokens in the [kNativeTokenAgeGroupReceived] group are
  // moved to the [kNativeTokenAgeGroupAlreadyScheduled] group. So this has the
  // visual effect of:
  //     [kNativeTokenAgeGroupReceived]
  // --> [kNativeTokenAgeGroupAlreadyScheduled]
  // --> [kNativeTokenAgeGroupToSchedule].
  //
  // Note that the average ages of these groups change when native tokens are
  // moved between them.
  static void cycle_groups(NativeTokenAge &current_averages);

  BlockHeight current_block() const
  {
    return current_block_.load(std::memory_order_relaxed);
  }
  x23::Async<void> increment_current_block()
  {
    current_block_.fetch_add(1, std::memory_order_relaxed);
    co_await prune_transaction_outcomes();
  }
  void set_current_block(BlockHeight height)
  {
    current_block_.store(height, std::memory_order_relaxed);
  }

  // Returns whether this transaction body is cached, including transactions
  // scheduled by a pending nomination. Callers must authenticate the submitted
  // signed encoding before treating an existing body as admitted.
  bool is_pending(const SignedTransaction &tx, const TransactionHash &tx_hash);

  // The number of txs in the scheduler, including both those of the runqueue
  // and those that are stranded.
  ssize_t size() const { return size_.load(std::memory_order_relaxed); }
  size_t tx_cache_size()
  {
    return tx_cache_size_.load(std::memory_order_relaxed);
  }

  // Used by [MempoolCoordinatorRpc] and [MempoolDbRpc] to synchronize
  // native-token age updates.
  x23::CoroLock pending_native_token_age_update_lock_;
  x23::CoroFutex<int64_t> transfer_finished_{-1};

  x23::CoroLock global_lock_;

private:
  friend class Scheduler_BatchCommittedOutcomesCrossChunks_Test;

  // Reuse the keyed hash calculated for shard selection during map probes
  // and eviction. The cached value is internal; transaction identity remains
  // the original SHA-256 hash.
  struct TransactionOutcomeKey {
    TransactionHash tx_hash;
    size_t cached_hash;

    explicit TransactionOutcomeKey(const TransactionHash &hash)
      : tx_hash(hash), cached_hash(SipHash24Hasher{}(hash))
    {}
    bool operator==(const TransactionOutcomeKey &other) const
    {
      return tx_hash == other.tx_hash;
    }
  };
  struct TransactionOutcomeHasher {
    size_t operator()(const TransactionOutcomeKey &key) const noexcept
    {
      return key.cached_hash;
    }
  };
  struct TransactionOutcomeEntry {
    TransactionOutcomeKey key;
    SchedulerTransactionOutcome outcome;
    // We record the height since we use the height to evict tx outcomes that
    // are at least [kTransactionOutcomeBlockLifetime] blocks old.
    BlockHeight processed_height;
  };

  static constexpr size_t kTransactionOutcomeShards = 64;
  static_assert(kMaxTransactionOutcomes % kTransactionOutcomeShards == 0);
  static constexpr size_t kMaxTransactionsPerShard =
      kMaxTransactionOutcomes / kTransactionOutcomeShards;
  static constexpr size_t kTransactionOutcomeChunkSize = 4096;
  struct TransactionOutcomeShard {
    x23::CoroLock lock;
    std::list<TransactionOutcomeEntry> order;
    std::unordered_map<TransactionOutcomeKey,
                       std::list<TransactionOutcomeEntry>::iterator,
                       TransactionOutcomeHasher>
        outcomes;
    // Admission only clears failed outcomes. Most shards contain successes
    // exclusively, so avoid locking and probing the large success cache in
    // that case. Writers update this count while holding [lock].
    std::atomic<size_t> failed_outcomes = 0;
  };
  static size_t transaction_outcome_shard_index(const TransactionHash &hash)
  {
    return SipHash24Hasher{}(hash) % kTransactionOutcomeShards;
  }

  static size_t
  transaction_outcome_shard_index(const TransactionOutcomeKey &key)
  {
    return key.cached_hash % kTransactionOutcomeShards;
  }

  struct TxKey {
    BlockHeight expiry_block_height;
    TransactionHash tx_hash;

    bool operator<(const TxKey &other) const noexcept
    {
      if (expiry_block_height != other.expiry_block_height) {
        return expiry_block_height < other.expiry_block_height;
      }
      // [std::array] implements lexicographic comparison.
      return tx_hash < other.tx_hash;
    }
  };

  struct TxValue {
    // The tx itself.
    SignedTransaction tx;
    // The hash of [tx]'s body, its key in [txs_by_body_].
    TransactionHash tx_hash;
    // The hash of [tx]'s exact signed encoding, its key in [txs_].
    SignedTransactionHash signed_tx_hash;
    // Every other encoding of this body that is cached, each in [txs_] under
    // its own signed hash but in neither [txs_by_body_] nor a runqueue. They
    // are the encodings [CachedVariant::kSubmitVetted] submissions displaced,
    // and they are erased with the body. Set only on the entry held in
    // [txs_by_body_].
    std::vector<SignedTransactionHash> other_signed_tx_hashes;
  };

  struct AccountState {
    NativeTokenAge native_token_age{};
    std::map<TxKey, std::shared_ptr<TxValue>> txs;
  };

  struct PendingNominationState {
    BlockID block_id = 0;
    std::unordered_map<ContractAddress, NativeTokenAge, SipHash24Hasher>
        native_token_ages;
    std::unordered_set<ContractAddress, SipHash24Hasher> touched_accounts;
    std::unordered_set<ContractAddress, SipHash24Hasher>
        accounts_excluded_from_block;

    PendingNominationState() { clear(); }

    void clear()
    {
      block_id = 0;
      native_token_ages.clear();
      touched_accounts.clear();
      accounts_excluded_from_block.clear();
    }
  };

  struct AccountKey {
    BlockHeight average_native_token_age;
    ContractAddress account;

    bool operator<(const AccountKey &other) const noexcept
    {
      if (average_native_token_age != other.average_native_token_age) {
        return average_native_token_age < other.average_native_token_age;
      }
      // [std::array] implements lexicographic comparison.
      return account < other.account;
    }
  };

  AccountKey get_account_key(const ContractAddress &account);

  x23::Async<bool> account_is_ready(const ContractAddress &account,
                                    bool remove_expired_txs);

  bool add_txs_helper(const SignedTransaction &tx,
                      const TransactionHash &tx_hash,
                      const SignedTransactionHash &signed_tx_hash,
                      CachedVariant on_variant);
  // Removes the cached tx whose body hash is [tx_hash] from [txs_by_body_] and
  // [txs_], along with every other encoding of it. The caller holds the lock of
  // the tx's account, as every cache writer does, so the entry read here is
  // current.
  void erase_cached_tx(const TransactionHash &tx_hash);
  // TODO(jhumphri): Give this a better name.
  x23::Async<void> cycle_accounts_during_schedule_helper(
      const std::unordered_set<ContractAddress, SipHash24Hasher> &accounts,
      const TxKey &lower_bound);

  void process_transfers_helper(const Delta &delta,
                                BlockHeight block_height) noexcept;
  static bool apply_committed_tx_native_token_age(
      const SignedTransaction &tx, NativeTokenAge &native_token_age,
      uint128_t &native_tokens_spent_by_this_account, BlockHeight block_height);

  std::pair<AccountState *, std::unique_lock<std::mutex>>
  get_account_state(const ContractAddress &address);

  void wait_for_pending_detached_coroutines()
  {
    while (pending_detached_coroutines_.load(std::memory_order_acquire) > 0) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
  }

  // This method will cycle the groups for [account] if doing so gives the
  // account an advantage in scheduling. This happens when (1) cycling increases
  // the average age of the [kNativeTokenAgeGroupToSchedule] group or (2) no txs
  // are currently on the runqueue for [account] and the
  // [kNativeTokenAgeGroupToSchedule] group does not have enough native tokens
  // to schedule a tx with a gas limit of [gas_limit].
  void cycle_if_needed(const ContractAddress &account, GasAmount gas_limit);
  static bool cycle_if_needed(NativeTokenAge &native_token_age,
                              GasAmount gas_limit);
  NativeTokenAge *get_native_token_age_from_pending_nomination_create_if_needed(
      AccountState &account_state, const ContractAddress &account);
  const NativeTokenAge &get_native_token_age_from_pending_nomination_if_exists(
      const AccountState &account_state, const ContractAddress &account) const;
  x23::Async<void> refresh_ready_accounts(
      const std::unordered_set<ContractAddress, SipHash24Hasher> &accounts);
  x23::Async<void> refresh_accounts_excluded_from_block();
  bool account_excluded_from_block(const ContractAddress &account) const;
  void erase_ready_account(const ContractAddress &account);
  x23::Async<void> prune_transaction_outcomes();
  // These helpers require [shard.lock].
  static void prune_transaction_outcomes_locked(TransactionOutcomeShard &shard,
                                                BlockHeight height);
  static void record_transaction_outcome_locked(
      TransactionOutcomeShard &shard, const TransactionOutcomeKey &key,
      const SchedulerTransactionOutcome &outcome, BlockHeight processed_height);
  static void erase_transaction_outcome_locked(
      TransactionOutcomeShard &shard,
      std::list<TransactionOutcomeEntry>::iterator entry);

  std::set<AccountKey> ready_accounts_;
  PendingNominationState pending_nomination_;

  // Tx cache, keyed by each tx's exact signed encoding. It holds one live
  // encoding per body, which [txs_by_body_] indexes by body hash, plus the
  // encodings vetted submissions displaced (see
  // [TxValue::other_signed_tx_hashes]).
  // Entries are removed when transactions expire, are discarded, or appear in
  // a committed/validated block. Shared ownership keeps an entry's
  // transaction alive for in-flight lookups after it is removed from here.
  //
  // Every write to the two maps happens under the tx's account lock, which
  // keeps them in step for each body. Lookups take no account lock and touch
  // one map.
  SynchronizedMap<SignedTransactionHash, std::shared_ptr<TxValue>,
                  SipHash24Hasher>
      txs_;
  SynchronizedMap<TransactionHash, std::shared_ptr<TxValue>, SipHash24Hasher>
      txs_by_body_;
  // Per-account scheduler state, including native-token ages and the account
  // runqueue.
  SynchronizedMap<ContractAddress, AccountState, SipHash24Hasher> accounts_;

  // The number of txs on the runqueue. [size_] is signed because it can
  // temporarily go below 0 in the following scenario:
  // 1. [add_txs()] adds txs to the runqueue, but does not yet increment
  //    [size_].
  // 2. [schedule()] removes txs from the runqueue.
  // 3. [schedule()] decrements [size_]. It is at this point that [size_] may
  //    now be less than 0.
  // 4. [add_txs()] increments [size_].
  std::atomic<ssize_t> size_ = 0;
  std::atomic<ssize_t> tx_cache_size_ = 0;
  std::atomic<BlockHeight> current_block_ = -1;

  // Each shard owns an equal share of the capacity and evicts its oldest
  // outcomes at that limit. Keyed hashing distributes transaction hashes.
  std::array<TransactionOutcomeShard, kTransactionOutcomeShards>
      transaction_outcome_shards_;

  // The RPC client that the scheduler uses to talk to the database. The
  // scheduler uses this to read/write the number of native tokens and
  // native-token ages for accounts.
  std::shared_ptr<db_sched_client_t> db_sched_client_;
  size_t max_native_token_ages_per_record_ = kMaxNativeTokenAgesPerRecord;

  std::atomic<ssize_t> pending_detached_coroutines_ = 0;
};

} // namespace hogd
