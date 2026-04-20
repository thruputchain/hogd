# Fix: DBAuth Intro Update Race in subscribe_to_auth_data_changes

**Date**: 2026-04-15
**Issue**: #1462
**Severity**: High

## Problem

`AuthServer::subscribe_to_auth_data_changes()` has two bugs in its
initialization of `current_height_futex_`:

1. **Fatal CHECK on raced state**: `CHECK_EQ(current_height_futex_.load(),
   kUninitializedHeight)` aborts the process if a concurrent
   `register_auth_data_updates()` call initialized the futex first.

2. **TOCTOU on the bare store**: Even if the CHECK were removed, the
   non-atomic `load(); ... store()` sequence can clobber a height that a
   racing `register_auth_data_updates()` wrote between the two operations.

The race window exists because `hogdb::dbauth_intro()` registers the auth
subscriber (making it eligible for forwarded updates) before returning the
intro height, and the intro RPC response and forwarded update RPC travel on
independent TCP connections with no causal ordering.

`register_auth_data_updates()` already handles first-initialization
correctly via a CAS spin loop (`kUninitializedHeight -> kInitializing ->
height`). `subscribe_to_auth_data_changes()` needs to use the same atomic
primitive but does not need the `kInitializing` intermediate state because
it performs no cache writes.

## Fix

Replace the `CHECK_EQ` + bare `store` in `subscribe_to_auth_data_changes()`
with an atomic `rmw` CAS that stores the intro height only if the futex is
still `kUninitializedHeight`. If a racing update already initialized it,
skip the store — the update established a valid baseline, and subsequent
updates continue from there with no height gap.

Add a `broadcast()` after a successful store. The original code omitted
this under the assumption that no one could be waiting during
initialization. That assumption is invalidated by the race: a concurrent
`register_auth_data_updates()` may be spinning in its wait loop.

### Before

```cpp
// auth_server.cc:149-159
BlockHeight current_height =
    co_await dbauth_client_.subscribe_to_auth_data_changes(card);
CHECK_EQ(current_height_futex_.load(), kUninitializedHeight);
if (current_height != -1) {
    current_height_futex_.store(current_height);
    info("initializing auth server to height {}", current_height);
    // There should be no one waiting on [current_height_futex_] ...
}
```

### After

```cpp
BlockHeight current_height =
    co_await dbauth_client_.subscribe_to_auth_data_changes(card);
if (current_height != -1) {
    bool initialized = current_height_futex_.rmw([&](auto &value) {
      if (value == kUninitializedHeight) {
        value = current_height;
        return true;
      }
      return false;
    });
    if (initialized) {
      info("initializing auth server to height {}", current_height);
      current_height_futex_.broadcast(&default_threadpool());
    } else {
      info("auth server already initialized to height {} by concurrent "
           "update, intro height was {}",
           current_height_futex_.load(), current_height);
    }
} else {
    info("not initializing auth server height");
}
```

## Why the height-gap invariant is preserved

- The intro height H is always <= the racing update's height X, because
  `add_full_auth_change_subscriber()` registers the subscriber before
  `get_expected_query_height()` reads the height, and any block committed
  after subscription generates a forwarded update.
- Skipping the intro height leaves the futex at X. The next update is X+1,
  satisfying `current_height + 1 == height`. No gap.
- Cache misses for keys not in the racing update's batch are served from
  the DB, which is at height >= X.

## Test

Add `AuthServerTest.DbAuthIntroUpdateRace` adapted from the PoC in
issue #1462. The test uses a blocking intro RPC to widen the race window
deterministically:

1. Start a mock DB server whose `dbauth_intro()` blocks after being called.
2. Launch `subscribe_to_auth_data_changes()` on a background thread.
3. Wait for the intro to start, then call `register_auth_data_updates()` to
   initialize the futex while intro is blocked.
4. Release the intro.
5. Verify that subscribe completes without crashing, and that the futex
   reflects the update's height (not the intro height).

This is a **non-death test** — the fix means the race is tolerated, not
fatal. The original PoC was an `EXPECT_DEATH` test; ours asserts successful
completion.

## Files changed

| File | Change |
|------|--------|
| `hogd/auth/auth_server.cc` | Replace CHECK_EQ + store with rmw CAS + broadcast |
| `hogd/auth/auth_server_test.cc` | Add `DbAuthIntroUpdateRace` test |
