# Auth-Publish Lock Scope Fix Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Eliminate the out-of-order auth-publish deadlock (issue #1475) by narrowing `AuthChangeSubscriptionManager::lock_` so it never covers the awaited outbound RPC.

**Architecture:** Encapsulate every access to `auth_db_clients_` (and therefore every use of `lock_`) in three small private helpers. `publish_auth_changes` and `add_full_auth_change_subscriber` call these helpers but never name the lock themselves. A regression test with a height-ordered mock auth server exercises the exact path that deadlocks today.

**Tech Stack:** C++20, `x23::Async` / `x23::CoroLock` / `x23::CoroGuard`, GoogleTest, custom `RpcServer` test harness.

**Spec:** `docs/superpowers/specs/2026-04-16-auth-publish-lock-scope-design.md`

---

## File Structure

- **Modify** `hogd/hogdb/auth_change_subscription_manager.h` — add three private helper declarations.
- **Modify** `hogd/hogdb/auth_change_subscription_manager.cc` — implement helpers; refactor `publish_auth_changes` and `add_full_auth_change_subscriber` to use them.
- **Modify** `hogd/hogdb/auth_subscription_test.cc` — add a `HeightOrderedMockAuthDBRpc`/`HeightOrderedMockAuthDBServer` plus `AuthSubscriptionTest.OutOfOrderAuthPublishProgresses` regression test.

No new files, no changes to `background_commit_worker.cc` or `auth_server.cc`.

---

### Task 1: Add the regression test (must fail before the fix)

**Files:**
- Modify: `hogd/hogdb/auth_subscription_test.cc`

Add a height-ordered mock server (inline in the test file, in the existing anonymous namespace area) and a new test case that reproduces the deadlock by publishing height 2 before height 1. Written TDD-style: the assertions describe post-fix correct behavior, so it must fail on `main` and pass after Task 2.

- [ ] **Step 1: Add the height-ordered mock RPC type and test**

Append the following to `hogd/hogdb/auth_subscription_test.cc` (after the existing tests, inside `namespace hogd { ... }`):

```cpp
namespace {

// Mirrors AuthServer's strict ordered-height gate: refuses to complete
// authdb_forward_key_changes(H) until H-1 has been processed. Used to
// demonstrate that concurrent publishes must not be serialized on the
// client side while one is parked on the server-side gate.
class HeightOrderedMockAuthDBRpc {
public:
  using rpc_service = AuthDB1;

  x23::Async<void> authdb_forward_key_changes(BlockHeight height,
                                              KeyChangeBatch /*key_changes*/)
  {
    active_calls_.fetch_add(1, std::memory_order_release);
    arrivals_.fetch_add(1, std::memory_order_release);
    last_arrived_height_.store(height, std::memory_order_release);

    while (processed_height_.load(std::memory_order_acquire) + 1 != height) {
      if (shutdown_.load(std::memory_order_acquire)) {
        active_calls_.fetch_sub(1, std::memory_order_release);
        co_return;
      }
      co_await default_threadpool().bg();
    }

    processed_height_.store(height, std::memory_order_release);
    completed_.fetch_add(1, std::memory_order_release);
    active_calls_.fetch_sub(1, std::memory_order_release);
  }

  int arrivals() const { return arrivals_.load(std::memory_order_acquire); }
  int completed() const { return completed_.load(std::memory_order_acquire); }
  int active_calls() const
  {
    return active_calls_.load(std::memory_order_acquire);
  }
  BlockHeight last_arrived_height() const
  {
    return last_arrived_height_.load(std::memory_order_acquire);
  }
  void shutdown() { shutdown_.store(true, std::memory_order_release); }

private:
  std::atomic<BlockHeight> processed_height_{0};
  std::atomic<BlockHeight> last_arrived_height_{0};
  std::atomic<int> arrivals_{0};
  std::atomic<int> completed_{0};
  std::atomic<int> active_calls_{0};
  std::atomic<bool> shutdown_{false};
};

static_assert(is_AuthDB1<HeightOrderedMockAuthDBRpc *, x23::MaybeAsync>);

class HeightOrderedMockAuthDBServer
  : public RpcServer<HeightOrderedMockAuthDBRpc> {
public:
  HeightOrderedMockAuthDBServer() : RpcServer(default_coropoll()) {}
};

} // namespace

TEST_F(AuthSubscriptionTest, OutOfOrderAuthPublishProgresses)
{
  HeightOrderedMockAuthDBServer mock_server;
  uint16_t port = mock_server.start_server(0);
  std::string server_address = std::format("localhost:{}", port);
  AuthCard ordered_auth_card = AuthCard{.protocol = version::kHogdProtocol(),
                                        .upid = {},
                                        .address_for_db = server_address.c_str(),
                                        .address_for_mempool = ""};
  ASSERT_TRUE(
      subscription_manager_.add_full_auth_change_subscriber(ordered_auth_card)());

  // Publish height 2 first. The mock will park it on its ordered-height gate
  // (since processed_height_ is still 0).
  {
    PerThread<KeyChangeBatch> key_changes;
    subscription_manager_.detached_publish_auth_changes(2, std::move(key_changes));
  }

  auto arrive_deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (mock_server.impl()->arrivals() < 1) {
    ASSERT_LT(std::chrono::steady_clock::now(), arrive_deadline)
        << "height 2 never reached the auth server";
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  ASSERT_EQ(mock_server.impl()->last_arrived_height(), 2);

  // Now publish height 1. Before the fix, height 1 cannot enter
  // publish_auth_changes because height 2 holds lock_ while parked on the
  // server's gate, and the test will time out below.
  {
    PerThread<KeyChangeBatch> key_changes;
    subscription_manager_.detached_publish_auth_changes(1, std::move(key_changes));
  }

  auto complete_deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (mock_server.impl()->completed() < 2) {
    ASSERT_LT(std::chrono::steady_clock::now(), complete_deadline)
        << "auth publishes deadlocked: arrivals="
        << mock_server.impl()->arrivals()
        << " completed=" << mock_server.impl()->completed();
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  EXPECT_EQ(mock_server.impl()->arrivals(), 2);
  EXPECT_EQ(mock_server.impl()->completed(), 2);

  // Post-condition safety net: flush any lingering coroutines (none expected
  // after completed == 2) before the server tears down.
  mock_server.impl()->shutdown();
  auto cleanup_deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (mock_server.impl()->active_calls() != 0) {
    ASSERT_LT(std::chrono::steady_clock::now(), cleanup_deadline)
        << "timed out waiting for detached publishers to exit during cleanup";
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
}
```

Also ensure the following headers are present at the top of the file (add any that are missing; leave existing includes in place):

```cpp
#include <atomic>
#include <chrono>
#include <thread>
```

- [ ] **Step 2: Build the test binary on main and confirm the test fails (proves the bug)**

Use the project `build` skill to build `hogdb_test` (the user insists on the build skill, never raw cmake). Then:

```bash
ctest --test-dir <build-dir> --output-on-failure --timeout 60 \
  -R "^AuthSubscriptionTest\.OutOfOrderAuthPublishProgresses$"
```

Expected: FAIL with the "auth publishes deadlocked: arrivals=1 completed=0" message (or a 60-second timeout).

- [ ] **Step 3: Commit the failing test**

```bash
git add hogd/hogdb/auth_subscription_test.cc
git commit -m "test(hogdb): regression test for out-of-order auth publish deadlock (#1475)"
```

---

### Task 2: Narrow `lock_` via three private helpers

**Files:**
- Modify: `hogd/hogdb/auth_change_subscription_manager.h`
- Modify: `hogd/hogdb/auth_change_subscription_manager.cc`

Encapsulate every use of `lock_` in three private helpers. `publish_auth_changes` and `add_full_auth_change_subscriber` never name `lock_` after this task.

- [ ] **Step 1: Add helper declarations to the header**

Edit `hogd/hogdb/auth_change_subscription_manager.h`. Inside the `private:` block (after the existing `create_client` declaration, before the data members), add:

```cpp
  // Under lock_: kick off authdb_forward_key_changes on every current client,
  // returning (card, future) pairs. A client whose dispatch throws
  // synchronously is erased from the map before returning. Lock is released
  // when this coroutine returns — the caller awaits the futures unlocked.
  x23::Async<std::vector<std::pair<AuthCard, x23::Async<void>>>>
  start_forward_key_changes(BlockHeight height, const KeyChangeBatch &batch);

  // Under lock_: erase one client. Used on post-await RPC failure.
  x23::Async<void> erase_client(const AuthCard &card);

  // Under lock_: replace-or-insert one client.
  x23::Async<void> insert_client(AuthCard card, auth_db_client_t client);
```

- [ ] **Step 2: Implement the three helpers in the `.cc`**

Edit `hogd/hogdb/auth_change_subscription_manager.cc`. After the existing `create_client` definition (and before the closing `} // namespace hogd`), add:

```cpp
x23::Async<std::vector<std::pair<AuthCard, x23::Async<void>>>>
AuthChangeSubscriptionManager::start_forward_key_changes(
    BlockHeight height, const KeyChangeBatch &batch)
{
  std::vector<std::pair<AuthCard, x23::Async<void>>> futures;

  x23::CoroGuard guard;
  co_await guard(lock_);

  futures.reserve(auth_db_clients_.size());
  for (auto it = auth_db_clients_.begin(); it != auth_db_clients_.end();) {
    auto &client = it->second;
    try {
      futures.emplace_back(it->first,
                           client.authdb_forward_key_changes(height, batch));
      ++it;
    } catch (x23::XdrException &e) {
      error("AuthChangeSubscriptionManager: Failed to forward key changes to "
            "auth server {}: {}.  Closing connection.",
            x23::show(it->first), e.what());
      it = auth_db_clients_.erase(it);
    }
  }
  co_return futures;
}

x23::Async<void>
AuthChangeSubscriptionManager::erase_client(const AuthCard &card)
{
  x23::CoroGuard guard;
  co_await guard(lock_);
  auth_db_clients_.erase(card);
}

x23::Async<void>
AuthChangeSubscriptionManager::insert_client(AuthCard card,
                                             auth_db_client_t client)
{
  x23::CoroGuard guard;
  co_await guard(lock_);
  auth_db_clients_.erase(card);
  [[maybe_unused]] auto [_, inserted] =
      auth_db_clients_.emplace(std::move(card), std::move(client));
  assert(inserted);
  co_return;
}
```

- [ ] **Step 3: Rewrite `publish_auth_changes` to use the helper**

Replace the existing body of `AuthChangeSubscriptionManager::publish_auth_changes` (currently lines 36-80 of the `.cc`) with:

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

- [ ] **Step 4: Rewrite the final critical section of `add_full_auth_change_subscriber`**

In `add_full_auth_change_subscriber`, replace the final block (currently the `CoroGuard guard; co_await guard(lock_); auth_db_clients_.erase(card); auto [_, inserted] = ...emplace(...); assert(inserted); co_return true;` segment, lines ~123-130) with:

```cpp
  co_await insert_client(std::move(card), std::move(*client));
  co_return true;
```

After this task, `lock_` should appear only inside `start_forward_key_changes`, `erase_client`, and `insert_client`. Verify with a grep.

- [ ] **Step 5: Verify the grep invariant**

```bash
```

Use the Grep tool for `lock_` in `hogd/hogdb/auth_change_subscription_manager.cc`. Expected: matches only inside the three helper function bodies (`start_forward_key_changes`, `erase_client`, `insert_client`). No match inside `publish_auth_changes` or `add_full_auth_change_subscriber`.

- [ ] **Step 6: Build and run the regression test**

Use the `build` skill to rebuild `hogdb_test`. Then:

```bash
ctest --test-dir <build-dir> --output-on-failure --timeout 60 \
  -R "^AuthSubscriptionTest\.OutOfOrderAuthPublishProgresses$"
```

Expected: PASS.

- [ ] **Step 7: Run the rest of the hogdb_test suite to confirm no regressions**

```bash
ctest --test-dir <build-dir> --output-on-failure --timeout 120 \
  -R "^AuthSubscriptionTest\."
```

Expected: all auth-subscription tests PASS (including the pre-existing `AuthChangesAreForwardedToSubscriber`).

- [ ] **Step 8: Commit the fix**

```bash
git add hogd/hogdb/auth_change_subscription_manager.h \
        hogd/hogdb/auth_change_subscription_manager.cc
git commit -m "fix(hogdb): narrow AuthChangeSubscriptionManager lock scope (#1475)"
```

---

## Self-Review Checklist

- Spec section "Fix" ↔ Task 2 Steps 1-4 (helpers + refactors).
- Spec section "Invariant" ↔ Task 2 Step 5 (grep check).
- Spec section "Regression Test" ↔ Task 1 (test added) and Task 2 Step 6 (confirmed passing post-fix).
- Spec "Out of Scope" — neither task touches `background_commit_worker.cc`, `auth_server.cc`, or `detached_publish_auth_changes`'s threadpool yield. ✓
- No placeholders, no "TBD", no references to undefined types.
- Method names consistent across tasks: `start_forward_key_changes`, `erase_client`, `insert_client`.
