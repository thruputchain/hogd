# ViewGateway Viewing BlockTx Design

**Date**: 2026-05-18
**Issue**: #2062 — ViewGateway self-tip DbTip returns InternalError

## Problem

`ViewGatewayRpc::run_view_attempt()` constructs `StorageSnapshotCache` and
`PerTransactionDatabaseProxy` using the committed `tip.id` as the block ID.
When a view contract calls `GetBlockHeight`, the path reaches
`DatabaseProxy::get_db_tip(tip.id)`, which receives the same `tip.id` back
from HogDB and rejects it as `Unrecoverable` — the check `x.id == block_id`
is a valid invariant during block execution (where `block_id` is a child
block and the returned tip should be its parent), but wrong for the view
gateway case where there is no child block.

The deeper problem: the ViewGateway bypasses the block lifecycle entirely. It
never opens a block transaction on HogDB. Normal execution goes through
`dbsched_create_snapshot(parent_id, new_id, mode)` which creates a `BlockTX`
in `RPCContext`, then uses `new_id` (distinct from the tip) for all proxy
objects. The ViewGateway should do the same.

Additionally, the current code creates a new `StorageSnapshotCache` on the
stack for every view request and discards it — wasting contract and
deployment cache lookups that should persist across requests.

## Design

### Long-lived viewing block tx shared across view requests

Follow the `ExecutorStateManager` pattern: the ViewGateway opens a viewing
block transaction (`kBlockExecutionModeViewing`) on `RPCContext` and shares
it — along with its `StorageSnapshotCache` — across all incoming view
requests until the block goes stale.

### View state and ownership

A `ViewState` struct holds:
- The viewing block's `BlockID`
- A `StorageSnapshotCache` initialized with that block ID

`ViewGatewayRpc` owns a pointer to the current `PerThreadARC<ViewState>`,
protected by a mutex that governs rotation. The `StorageSnapshotCache`'s
contract and deployment caches persist across block tx rotations (only
`block_id_`, `db_tip_`, and `snapshot_item_cache_` reset via
`set_block_id`).

### Request flow

1. Incoming view request calls `try_acquire_reference()` on the active
   `PerThreadARC<ViewState>`. If it fails (shutdown in progress), retry to
   acquire the (potentially new) state.
2. The view runs against the shared `StorageSnapshotCache`, constructing a
   per-request `PerTransactionDatabaseProxy(db_proxy_, view_block_id, ...)`
   and `PerTransactionEventServerProxy(event_server_proxy_, view_block_id,
   ...)` using the viewing block ID from the acquired state.
3. On completion, call `release_reference()`. If it returns true, this thread
   is the last one out after shutdown — GC the old state (call
   `ctx_.remove_shutdown_block_tx(old_block_id)`, delete the old
   `PerThreadARC<ViewState>`).

### Block tx rotation on Abandon (no waiting)

When a view returns `Abandoned`:

1. Grab the rotation mutex.
2. Check if the current view state is still the one this thread was using. If
   someone already rotated, release the mutex, release the ref on the old
   state (GC if last out), and retry with the new state.
3. If still stale: call `initiate_shutdown()` on the old
   `PerThreadARC<ViewState>`.
4. Immediately generate a new block ID, open a new viewing block tx via
   `ctx_.open_block_tx(new_id, kBlockExecutionModeViewing)`, create a new
   `PerThreadARC<ViewState>` with the new block ID and a reset
   `StorageSnapshotCache`, and publish it as the current state.
5. Release the rotation mutex.
6. Release the ref on the old state. If `release_reference()` returns true,
   this thread is last out — GC the old state
   (`remove_shutdown_block_tx(old_block_id)`, delete old arc).
7. Retry the view against the new state.

Other threads still in-flight against the old state will eventually get
Abandon (or finish normally). Their `release_reference()` drains the old
arc; whichever thread gets `true` performs the cleanup. No thread waits for
the drain.

### Block ID generation

Use the same timestamp-nanosecond atomic pattern as
`MempoolCoordRpc::generate_block_id()`. Extract to a shared utility or
duplicate locally in `ViewGatewayRpc`.

### Lazy initialization

The first viewing block tx is opened on the first view request, not at
`ViewGatewayRpc` construction time. This avoids opening a block tx before
HogDB has a committed tip.

### Snapshot cache lifecycle

Each `ViewState` owns its own `StorageSnapshotCache`. On rotation, the new
`ViewState` gets a fresh `StorageSnapshotCache` — the old one is destroyed
when the old `ViewState` is GC'd. This is simpler than trying to move the
cache across states (the old state can't be dismantled until its refcount
drains, and the new state needs a cache immediately). The contract and
deployment caches re-warm quickly under normal view traffic.

## Files to change

- `hogd/hogexec/interface/rpc_from_view_gateway.h` — Add `ViewState` struct,
  `PerThreadARC<ViewState>*` member, rotation mutex,
  `generate_block_id()`.
- `hogd/hogexec/interface/rpc_from_view_gateway.cc` — Rewrite
  `run_view_with_retries` and `run_view_attempt` to acquire a ref on the
  shared view state, use its block ID and snapshot cache, and handle
  rotation on Abandon. Update or fold `run_view_with_retries_for_gateway`.
- `hogd/hogexec/runtime/view/view_test.cc` — Add test exercising
  `GetBlockHeight` through a ViewGateway-style view (the PoC from #2062,
  updated to open a proper viewing block tx).
