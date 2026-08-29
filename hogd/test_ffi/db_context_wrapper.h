#pragma once

#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <optional>

#include "auth_rpc.hh"
#include "contract.hh"
#include "db_sched.hh"
#include "event.hh"
#include "hogd/base/log.h"
#include "hogd/base/rpc_server.h"
#include "hogd/hog_auth/auth_server.h"
#include "hogd/hog_exec/runtime/event_server_proxy.h"
#include "hogd/hog_exec/runtime/item/database_proxy.h"
#include "hogd/hog_exec/runtime/item/storage_snapshot_cache.h"
#include "hogd/hog_exec/runtime/test_util/hog_db_wrapper.h"
#include "hogd/hog_exec/runtime/test_util/test_event_aggregator.h"
#include "network_config.hh"
#include "rpc_types.hh"
#include "storage_cost.hh"
#include "transaction.hh"
#include "x23/srpc.h"
#include "x23/vec.h"

namespace hogd {

// Database-related context for the test environment.
// Provides appropriate methods for manipulating the evolution
// of the database during a test.
class DBContext {
public:
  // `genesis_root_contract` is installed (without artifact verification) as
  // the genesis root deployment; it can be any contract type genesis
  // validation accepts, a mock contract included. Test consumers execute
  // contract bodies through the mock trampoline, so the contract's code
  // never runs -- only the deployment exists.
  //
  // `network_config` is the chain's execution parameters; the protocol sets
  // no value for any of them, so a caller that does not care can pass
  // stock_network_config().
  DBContext(StorageCostModelXdr const &cost_model, NetworkHash network_hash,
            Contract const &genesis_root_contract,
            NetworkConfig const &network_config);

  void advance_new_block(BlockExecutionMode mode =
                             BlockExecutionMode::kBlockExecutionModeProposal);

  struct EphemeralTxIdentifier {
    BlockID block_id;
    TransactionID tx_id;
  };

  StorageSnapshotCache &get_snapshot()
  {
    assert(snapshot_cache_);
    return *snapshot_cache_;
  }

  EphemeralTxIdentifier allocate_identifier()
  {
    return EphemeralTxIdentifier{.block_id = current_block_id_,
                                 .tx_id = allocate_txid()};
  }

  PerTransactionDatabaseProxy get_new_db_proxy(EphemeralTxIdentifier identifier)
  {
    return PerTransactionDatabaseProxy(*db_proxy_, identifier.block_id,
                                       identifier.tx_id);
  }

  PerTransactionEventServerProxy
  get_new_event_server_proxy(EphemeralTxIdentifier identifier)
  {
    return PerTransactionEventServerProxy(*event_server_proxy_,
                                          identifier.block_id,
                                          identifier.tx_id);
  }

  BlockID get_current_block_id() const { return current_block_id_; }

  // Test FFI lifecycle admission control. A SyscallHandlerContext borrows this
  // DBContext, along with the snapshot cache and DB/event proxies derived from
  // it, by reference. Freeing the DB or advancing it to a new block while a
  // handler is still open would dangle those references, so the test FFI layer
  // rejects those operations while a handler is open (issues #2433, #2436).
  //
  // The two sides are mutually exclusive, not merely counted: a plain "are
  // there handlers?" load only supports check-then-act, so a handler opening
  // between a lifecycle operation's zero check and its replacement of borrowed
  // state was admitted against state the operation then destroyed (issue
  // #3049). lifecycle_state_ is a small state machine that linearizes the two:
  // a lifecycle transition is admitted only from a state with no open handlers
  // and no other transition in flight, and while one is in flight handler
  // admission is refused.

  // Registers a handler's borrow of this DBContext. Returns false when a
  // lifecycle transition is in flight; the caller must not borrow DB state in
  // that case. Must be paired with unregister_open_handler().
  [[nodiscard]] bool try_register_open_handler()
  {
    auto state = lifecycle_state_.load(std::memory_order_relaxed);
    do {
      if (state & kLifecycleTransitionBit) {
        return false;
      }
    } while (
        !lifecycle_state_.compare_exchange_weak(state, state + 1,
                                                std::memory_order_acq_rel,
                                                std::memory_order_relaxed));
    return true;
  }

  void unregister_open_handler()
  {
    [[maybe_unused]] auto prev =
        lifecycle_state_.fetch_sub(1, std::memory_order_release);
    // A transition cannot start while a handler is open, so it cannot be in
    // flight here and the subtraction cannot touch the transition bit.
    assert((prev & kLifecycleTransitionBit) == 0);
    assert((prev & kOpenHandlerCountMask) > 0);
  }

  bool has_open_handlers() const
  {
    return (lifecycle_state_.load(std::memory_order_acquire) &
            kOpenHandlerCountMask) > 0;
  }

  // Exclusive DB lifecycle transition (advance_new_block(), free_db()): while
  // one is held, handler admission is refused, so state the transition
  // replaces or destroys cannot acquire new borrowers. held() reports whether
  // the transition was admitted -- it is refused while any handler is open or
  // another transition is in flight.
  class LifecycleTransition {
  public:
    explicit LifecycleTransition(DBContext &db)
      : db_(&db), held_(db.try_begin_lifecycle_transition())
    {}
    ~LifecycleTransition()
    {
      if (held_) {
        db_->end_lifecycle_transition();
      }
    }

    LifecycleTransition(LifecycleTransition const &) = delete;
    LifecycleTransition &operator=(LifecycleTransition const &) = delete;
    LifecycleTransition(LifecycleTransition &&) = delete;
    LifecycleTransition &operator=(LifecycleTransition &&) = delete;

    bool held() const { return held_; }

    // Drop the transition without ending it, for a caller that destroys the
    // DBContext under it (free_db()): there is no object left to release it
    // on.
    void release_without_ending() { held_ = false; }

  private:
    DBContext *db_;
    bool held_;
  };

  // Poison the DB's lifecycle. Set when a handler close failed to confirm
  // closure of its HogDb transaction (issue #2596): the open-handler guard has
  // already been released by the handler's destruction, but an InvokeTX may
  // still hold a BlockTX open, so advancing the block could drive the scheduler
  // over orphaned transaction state. Once poisoned, advance_new_block() refuses
  // to run; free_db() (full teardown) stays the only safe lifecycle operation.
  // One-way and atomic to match lifecycle_state_.
  void poison_lifecycle() { lifecycle_poisoned_.store(true); }
  bool is_lifecycle_poisoned() const { return lifecycle_poisoned_.load(); }

  x23::Async<std::optional<TxEventLog>>
  get_tx_event_log(EphemeralTxIdentifier identifier)
  {
    return event_server_.get_events(identifier.block_id, identifier.tx_id);
  }

  auto &get_base_db() { return hog_db_wrapper_; }

  // The execution parameters in force, read back off the committed tip -- the
  // same place the executor reads them from.
  NetworkConfig get_network_config()
  {
    auto dbtip = get_base_db().get_db().read_dbtip();
    CHECK(dbtip.has_value());
    return dbtip->network_config;
  }

  std::optional<WireStorageItem>
  get_snapshot_item(AddressAndKey const &addrkey);

  AuthErrorCode run_auth_checks(SignedTransaction const &tx)
  {
    x23::span<const SignedTransaction> txs(&tx, 1);
    auto dbtip = get_base_db().get_db().read_dbtip();
    CHECK(dbtip.has_value());
    auto res = auth_server_->verify_transactions(
        dbtip->value.proposal_header.block_height, txs)();
    CHECK_EQ(res.size(), 1U);
    return res[0];
  }

private:
  TransactionID allocate_txid() { return current_tx_id_.fetch_add(1); }

  // Admitted only from the idle state: no handler open, no other transition in
  // flight. See LifecycleTransition, which is how callers take one.
  [[nodiscard]] bool try_begin_lifecycle_transition()
  {
    std::uint32_t idle = 0;
    return lifecycle_state_.compare_exchange_strong(idle,
                                                    kLifecycleTransitionBit,
                                                    std::memory_order_acq_rel,
                                                    std::memory_order_relaxed);
  }

  void end_lifecycle_transition()
  {
    lifecycle_state_.store(0, std::memory_order_release);
  }

  using db_sched_client_t =
      DbSched1::client<x23::SRpcStream<x23::RpcLogger>::ClientRef>;

  BlockID current_block_id_ = 0;

  HogdbWrapper hog_db_wrapper_;

  std::unique_ptr<AuthServer> auth_server_;
  RpcServer<AuthDBRpc> auth_db_server_;

  db_sched_client_t db_sched_client_;

  TestEventAggregator event_server_;
  TestEventAggregatorExecServer event_exec_server_ =
      TestEventAggregatorExecServer(event_server_);

  std::unique_ptr<DatabaseProxy> db_proxy_;
  std::unique_ptr<EventServerProxy> event_server_proxy_;
  std::unique_ptr<StorageSnapshotCache> snapshot_cache_ = nullptr;
  std::atomic<TransactionID> current_tx_id_ = 0;

  // Lifecycle admission state: the number of SyscallHandlerContexts currently
  // borrowing this DBContext in the low bits, plus a flag for an in-flight
  // exclusive lifecycle transition in the high bit. Kept in one word so
  // admitting a handler and admitting a transition are a single compare and
  // exchange each, and neither can be admitted against a stale observation of
  // the other. See try_register_open_handler() and LifecycleTransition.
  static constexpr std::uint32_t kLifecycleTransitionBit = 1U << 31;
  static constexpr std::uint32_t kOpenHandlerCountMask =
      ~kLifecycleTransitionBit;
  std::atomic<std::uint32_t> lifecycle_state_ = 0;
  // See poison_lifecycle().
  std::atomic<bool> lifecycle_poisoned_ = false;
};

} // namespace hogd
