#pragma once

#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <optional>
#include <stdexcept>

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
#include "x23/async.h"
#include "x23/mvar.h"
#include "x23/srpc.h"
#include "x23/vec.h"

namespace hogd {

// A block was advanced with a timepoint earlier than the block it follows.
// Its own type so the FFI layer can report a caller's bad argument as one,
// rather than folding it in with the internal failures advancing can hit.
class BackwardsTimepointError : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
};

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

  // Height is the tip's plus one, read off the tip rather than a block id.
  // The timepoint is whatever the advance that opened this block was given;
  // unlike the height it is not derivable from the chain.
  BlockExecutionContext block_execution_context() const;

  // Opens the next block. `block_timepoint` is that block's timepoint in Unix
  // nanoseconds; nullopt takes the current wall clock, which is what a caller
  // with no opinion wants -- a contract reading GetBlockTimepoint then sees a
  // plausible present-day value rather than a fixed one. Pass a value to pin
  // it, which is how a test drives contract logic keyed on time: deadlines,
  // expiry, vesting, rate limits.
  //
  // A block may not be older than the one it follows: a timepoint earlier
  // than the open block's throws BackwardsTimepointError and advances
  // nothing, so a refused advance leaves the DB exactly as it was. Equal is
  // allowed -- two blocks sharing a nanosecond is implausible rather than
  // incoherent. The wall clock is checked the same way as a pinned value; it
  // is not normally capable of going backwards, and quietly clamping it would
  // hide the clock stepping under a running test.
  void advance_new_block(
      BlockExecutionMode mode = BlockExecutionMode::kBlockExecutionModeProposal,
      std::optional<TimePoint> block_timepoint = std::nullopt);

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
  // The two sides are mutually exclusive, not merely counted. An open-handler
  // count on its own only supports check-then-act, so a handler opening
  // between a lifecycle operation's zero check and its replacement of borrowed
  // state was admitted against state the operation then destroyed (issue
  // #3049). Both sides now decide under lifecycle_state_'s lock: a transition
  // is admitted only with no handler open and no other transition in flight,
  // and a handler only with no transition in flight. Entry points that touch
  // DB state without owning a handler borrow for the length of that access
  // (see Borrow; issues #2704, #2727, #2667).

  // Registers a handler's borrow of this DBContext. Returns false when a
  // lifecycle transition is in flight; the caller must not borrow DB state in
  // that case. Must be paired with unregister_open_handler().
  [[nodiscard]] bool try_register_open_handler()
  {
    return lifecycle_state_.with([](LifecycleState &state) {
      if (state.transition_in_flight) {
        return false;
      }
      ++state.open_handlers;
      return true;
    });
  }

  void unregister_open_handler()
  {
    lifecycle_state_.with([](LifecycleState &state) {
      assert(state.open_handlers > 0);
      --state.open_handlers;
    });
  }

  // Scoped borrow for an entry point that touches DB state without owning a
  // handler: a snapshot read, a setup_* write (a base-DB put() must not overlap
  // the sync() an advance commits with), or a handler open ahead of its guard.
  // Non-throwing, so a noexcept FFI entry point can turn a refusal into a
  // structured error.
  class Borrow {
  public:
    explicit Borrow(DBContext &db)
      : db_(&db), held_(db.try_register_open_handler())
    {}
    ~Borrow()
    {
      if (held_) {
        db_->unregister_open_handler();
      }
    }

    Borrow(Borrow const &) = delete;
    Borrow &operator=(Borrow const &) = delete;
    Borrow(Borrow &&) = delete;
    Borrow &operator=(Borrow &&) = delete;

    bool held() const { return held_; }

  private:
    DBContext *db_;
    bool held_;
  };

  bool has_open_handlers() const { return lifecycle_state_->open_handlers > 0; }

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
  // One-way and atomic so a failing close can set it without taking the
  // lifecycle lock.
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
    return lifecycle_state_.with([](LifecycleState &state) {
      if (state.open_handlers > 0 || state.transition_in_flight) {
        return false;
      }
      state.transition_in_flight = true;
      return true;
    });
  }

  void end_lifecycle_transition()
  {
    lifecycle_state_->transition_in_flight = false;
  }

  using db_sched_client_t =
      DbSched1::client<x23::SRpcStream<x23::RpcLogger>::ClientRef>;

  // Unix nanoseconds, as a real chain's header carries. Set by each
  // advance_new_block() and read back by block_execution_context(): for an
  // open block the timepoint is not on the tip (that one is the parent's), so
  // unlike the height there is nothing to recompute it from.
  TimePoint block_timepoint_ = 0;

  BlockID current_block_id_ = 0;
  // The height of the last committed block; genesis commits at 0 before the
  // first advance. Tracked rather than read off the tip so building the open
  // block's execution context costs no rpc, and so nothing here assumes
  // block ids equal heights.
  BlockHeight committed_height_ = 0;
  // The network this chain belongs to. Every committed value carries it, not
  // just genesis: the commit path replaces the persisted tip wholesale, so a
  // value that omits it puts the chain back on network zero.
  NetworkHash network_hash_;

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

  struct LifecycleState {
    // SyscallHandlerContexts currently borrowing this DBContext.
    int open_handlers = 0;
    // An advance_new_block() or free_db() is running.
    bool transition_in_flight = false;
  };

  // Handler admission and lifecycle transitions both decide under this lock,
  // so neither can be admitted against a stale observation of the other. See
  // try_register_open_handler() and LifecycleTransition. Mutable so the
  // const has_open_handlers() can take the lock.
  mutable x23::MVar<LifecycleState> lifecycle_state_;
  // See poison_lifecycle().
  std::atomic<bool> lifecycle_poisoned_ = false;
};

} // namespace hogd
