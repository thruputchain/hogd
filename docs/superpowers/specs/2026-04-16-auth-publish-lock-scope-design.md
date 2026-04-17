# Auth-Publish Lock Scope Fix — Design

**Date:** 2026-04-16
**Issue:** [#1475 — Out-of-Order Auth Publish Deadlocks Height Progression](https://github.com/groundhogchain/hogd/issues/1475)
**Subsystem:** `hogd/hogdb` (auth change subscription)

## Problem

`AuthChangeSubscriptionManager::publish_auth_changes` holds `lock_` across the awaited outbound `authdb_forward_key_changes` RPC. Combined with `BackgroundCommitWorker` firing `detached_publish_auth_changes` and `detached_publish_auth_changes` yielding to the thread pool before publishing, two adjacent block heights can reach `publish_auth_changes` out of order:

1. Height `H+1` acquires `lock_` first, invokes `authdb_forward_key_changes(H+1)`, parks on the auth server's `current_height + 1 == height` futex gate.
2. Height `H` cannot enter `publish_auth_changes` because `lock_` is held.
3. Height `H+1` will never wake until height `H` arrives. Deadlock; mempool blocks on `wait_for_auth_data(H)` forever.

The auth server already reconstructs FIFO order on its side (via the futex). The only reason the deadlock exists is that the publisher lock serializes concurrent publishes *on the client side* while one is parked on the server. Released concurrently, the server handles out-of-order arrivals correctly.

## Fix

Narrow `lock_` so it covers only `auth_db_clients_` map access, never the awaited network call. Rather than sprinkle `CoroGuard` blocks through `publish_auth_changes`, encapsulate every use of `lock_` inside small private helpers. The public methods never name the lock.

### New private helpers on `AuthChangeSubscriptionManager`

```cpp
// Under lock: kick off forward RPCs for every current client and return the
// (card, future) pairs. Clients whose synchronous RPC dispatch throws are
// erased from the map before returning.
x23::Async<std::vector<std::pair<AuthCard, x23::Async<void>>>>
start_forward_key_changes(BlockHeight height, const KeyChangeBatch &batch);

// Under lock: erase one client. Used after a post-await RPC failure.
x23::Async<void> erase_client(const AuthCard &card);

// Under lock: replace-or-insert one client. Used by
// add_full_auth_change_subscriber.
x23::Async<void> insert_client(AuthCard card, auth_db_client_t client);
```

Each helper acquires `lock_` via `CoroGuard`, does its map work, and returns — the lock is released on return.

### Revised `publish_auth_changes`

```cpp
x23::Async<void>
AuthChangeSubscriptionManager::publish_auth_changes(
    BlockHeight height, PerThread<KeyChangeBatch> key_changes)
{
  KeyChangeBatch combined_batch;
  {
    auto ul = key_changes.lock_threads();
    key_changes.for_each(ul, [&](const KeyChangeBatch &batch) {
      combined_batch.append_range(batch);
    });
  }

  auto futures = co_await start_forward_key_changes(height, combined_batch);

  for (auto &[card, future] : futures) {
    try {
      co_await future;
    } catch (const x23::XdrException &e) {
      error("AuthChangeSubscriptionManager: Bad response from auth server {}: "
            "{}.  Closing connection.",
            x23::show(card), e.what());
      co_await erase_client(card);
    }
  }
}
```

### Revised `add_full_auth_change_subscriber`

The final critical section (erase + emplace under `lock_`) becomes:

```cpp
co_await insert_client(std::move(card), std::move(*client));
co_return true;
```

## Invariant

After this change, `lock_` is named only inside the three private helpers. This is trivially grep-enforceable and makes it impossible to reintroduce the deadlock by extending `publish_auth_changes` in the future.

## Concurrency Notes

- Multiple publishes may be in flight on the same `auth_db_client_t` at once (e.g., heights `H` and `H+1` both awaiting `authdb_forward_key_changes`). The `RpcClient` multiplexes independent calls; the auth server's `current_height_futex_` serializes application.
- A subscriber can be added or replaced while a prior publish is still mid-await. The prior publish already captured its `x23::Async<void>` by value before releasing the lock, so mutating the map does not affect in-flight awaits.
- The stale-drop path in `AuthServer::register_auth_data_updates` (dropping when `current_height >= height`) is not reachable from this path: `BackgroundCommitWorker` submits heights monotonically, each exactly once.

## Regression Test

Add the mock and test to `hogd/hogdb/auth_subscription_test.cc`, based on the PoC from issue #1475 but with assertions inverted so the test passes only when the bug is fixed:

- Test name: `AuthSubscriptionTest.OutOfOrderAuthPublishProgresses` (renamed from the issue's reproducer name, which described the buggy symptom).
- Mechanism: a `HeightOrderedMockAuthDBServer` that refuses to complete `authdb_forward_key_changes(H)` until `H-1` has been processed, exactly mirroring the real auth server's futex gate.
- Sequence:
  1. Publish height 2. Wait for it to *arrive* at the mock (it will park on the ordered gate).
  2. Publish height 1.
  3. Wait up to a few seconds for `completed == 2`.
- Assertions (regression-style — succeed after the fix, fail before it):
  - `arrivals == 2` — both publishes reach the mock server.
  - `completed == 2` — both publishes finish (height 1 unblocks height 2 via the mock's ordered gate).
  - `active_calls == 0` at test end, without needing to invoke `shutdown()`.
- Before the fix: height 1 cannot enter `publish_auth_changes` because height 2 holds `lock_`; `arrivals` stays at 1, `completed` stays at 0, the `completed == 2` wait times out, test fails.
- After the fix: both publishes run concurrently; the mock gates height 2 on height 1 and both complete in order.

The `shutdown()` escape hatch on the mock remains, used only as a post-assertion safety net so a failing run cannot wedge the test binary.

## Out of Scope

- No change to `BackgroundCommitWorker`. Commit-path overhead is intentionally kept minimal; publication remains fire-and-forget via `x23::Detached`.
- No change to `AuthServer`. Its ordered-height gate is correct and stays as-is.
- No change to `detached_publish_auth_changes`' `co_await default_threadpool().bg()`; reordered arrival is now tolerated by design.
