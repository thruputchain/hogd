# DBAuth Intro Race Fix Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Fix the race between `subscribe_to_auth_data_changes` and `register_auth_data_updates` that crashes the auth server on startup (issue #1462).

**Architecture:** Replace the unconditional `CHECK_EQ` + bare store in `subscribe_to_auth_data_changes` with an atomic `rmw` CAS + broadcast, matching the primitive already used by `register_auth_data_updates`. Add a non-death regression test that deterministically reproduces the race using a blocking intro mock.

**Tech Stack:** C++, x23 coroutines, gtest

---

### Task 1: Add the regression test

The test goes first so we can confirm it reproduces the crash before applying the fix.

**Files:**
- Modify: `hogd/auth/auth_server_test.cc`

- [ ] **Step 1: Add the `BlockingIntroDBAuthRpc` helper and `DbAuthIntroUpdateRace` test**

Insert before the closing `} // namespace hogd` at the bottom of `hogd/auth/auth_server_test.cc` (before line 302), add the following. The test is initially written as `EXPECT_DEATH` to confirm the bug exists. We will flip it to a non-death test in Task 2 after applying the fix.

Note: the test needs additional includes at the top of the file: `<format>` and `"hogd/base/rpc_server.h"`.

Add these includes after the existing includes (after line 11, before line 13):

```cpp
#include <format>

#include "hogd/base/rpc_client.h"
#include "hogd/base/rpc_server.h"
```

Then insert the test before the closing `} // namespace hogd` (replacing the `// NOLINTEND` line temporarily — we'll put it back in the right place):

```cpp
// NOLINTEND(bugprone-unchecked-optional-access)

namespace {

class BlockingIntroDBAuthRpc {
public:
  using rpc_service = DBAuth1;

  x23::Async<DBAuthResult> authdb_get_auth_data(BlockHeight, ContractAddress)
  {
    co_return DBAuthResult(DBAuthError::kDBAuthNotFound);
  }

  x23::Async<DBAuthResultBatch>
  authdb_get_auth_data_batch(BlockHeight, ContractAddressBatch const &addresses)
  {
    DBAuthResultBatch result;
    result.reserve(addresses.size());
    for (size_t i = 0; i < addresses.size(); ++i) {
      result.emplace_back(DBAuthError::kDBAuthNotFound);
    }
    co_return result;
  }

  x23::Async<DBAuthIntroResult> dbauth_intro(AuthCard)
  {
    intro_started_.store(true);
    intro_started_.notify_all();
    release_intro_.wait(false);

    DBAuthIntroResult ret(DBAuthIntroOutcome::kIntroSuccess);
    ret.current_height() = return_height_;
    co_return ret;
  }

  void wait_for_intro() { intro_started_.wait(false); }

  void release_intro(BlockHeight return_height)
  {
    return_height_ = return_height;
    release_intro_.store(true);
    release_intro_.notify_all();
  }

private:
  std::atomic<bool> intro_started_ = false;
  std::atomic<bool> release_intro_ = false;
  BlockHeight return_height_ = -1;
};

static_assert(is_DBAuth1<BlockingIntroDBAuthRpc *, x23::Async>);

} // namespace

TEST_F(AuthServerTest, DbAuthIntroUpdateRace)
{
  GTEST_FLAG_SET(death_test_style, "threadsafe");

  EXPECT_DEATH(
      ([] {
        RpcServer<BlockingIntroDBAuthRpc> db_server;
        const uint16_t db_port = db_server.start_server(0);
        const std::string db_address = std::format("localhost:{}", db_port);
        dbauth_client_t dbauth_client = make_dbauth_client(db_address)();
        AuthServer race_auth_server(std::move(dbauth_client), 16);

        AuthCard auth_card{.protocol = {},
                           .upid = {},
                           .address_for_db = "",
                           .address_for_mempool = ""};

        std::jthread subscribe_thread([&] {
          race_auth_server.subscribe_to_auth_data_changes(auth_card)();
        });

        db_server->impl()->wait_for_intro();

        x23::Ptr<AddressAuthData> auth_data_ptr;
        auth_data_ptr.emplace(make_test_auth_data(1));

        KeyChangeBatch key_changes;
        key_changes.push_back(KeyChange{.address = make_test_address(99),
                                        .auth_data = std::move(auth_data_ptr)});
        race_auth_server.register_auth_data_updates(1,
                                                    std::move(key_changes))();

        db_server->impl()->release_intro(1);
        subscribe_thread.join();
      }()),
      "");
}

} // namespace hogd
```

- [ ] **Step 2: Build and run the test to confirm it reproduces the crash**

Build using the VSCode build config (target: `auth_test`, preset: Debug).

Then run:
```bash
ctest --test-dir ~/build/hogd-debug --output-on-failure --timeout 120 -R "^AuthServerTest\.DbAuthIntroUpdateRace$"
```

Expected: PASS (the `EXPECT_DEATH` succeeds because the process aborts on `CHECK_EQ`).

- [ ] **Step 3: Commit the death test**

```bash
git add hogd/auth/auth_server_test.cc
git commit -m "test: add death test reproducing dbauth intro race (#1462)

The test confirms that subscribe_to_auth_data_changes crashes when
register_auth_data_updates initializes the futex first. Will be
converted to a non-death test after the fix is applied."
```

### Task 2: Apply the fix and convert the test

**Files:**
- Modify: `hogd/auth/auth_server.cc:145-164`
- Modify: `hogd/auth/auth_server_test.cc` (the test from Task 1)

- [ ] **Step 1: Replace the CHECK_EQ + store with rmw CAS + broadcast**

In `hogd/auth/auth_server.cc`, replace lines 149-163 (the body of `subscribe_to_auth_data_changes` after the opening brace) with:

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
  co_return;
```

- [ ] **Step 2: Convert the death test to a non-death test**

In `hogd/auth/auth_server_test.cc`, replace the `DbAuthIntroUpdateRace` test body with a non-death version that asserts successful completion:

```cpp
TEST_F(AuthServerTest, DbAuthIntroUpdateRace)
{
  RpcServer<BlockingIntroDBAuthRpc> db_server;
  const uint16_t db_port = db_server.start_server(0);
  const std::string db_address = std::format("localhost:{}", db_port);
  dbauth_client_t dbauth_client = make_dbauth_client(db_address)();
  AuthServer race_auth_server(std::move(dbauth_client), 16);

  AuthCard auth_card{.protocol = {},
                     .upid = {},
                     .address_for_db = "",
                     .address_for_mempool = ""};

  std::jthread subscribe_thread([&] {
    race_auth_server.subscribe_to_auth_data_changes(auth_card)();
  });

  db_server->impl()->wait_for_intro();

  x23::Ptr<AddressAuthData> auth_data_ptr;
  auth_data_ptr.emplace(make_test_auth_data(1));

  KeyChangeBatch key_changes;
  key_changes.push_back(KeyChange{.address = make_test_address(99),
                                  .auth_data = std::move(auth_data_ptr)});
  race_auth_server.register_auth_data_updates(1, std::move(key_changes))();

  db_server->impl()->release_intro(1);
  subscribe_thread.join();

  // The racing update should have won initialization — verify by checking
  // that a lookup at height 1 succeeds (futex is at height 1).
  auto result =
      race_auth_server.lookup_auth_data(1, make_test_address(99))();
  ASSERT_TRUE(result.has_value());
  ASSERT_NE(result.value(), nullptr);
  EXPECT_EQ(result.value()->first, 1);
  ASSERT_TRUE(result.value()->second.has_value());
}
```

- [ ] **Step 3: Build and run the full auth_test suite**

Build using the VSCode build config (target: `auth_test`, preset: Debug).

Then run:
```bash
ctest --test-dir ~/build/hogd-debug --output-on-failure --timeout 120 -R "^AuthServerTest\."
```

Expected: ALL PASS — the new test passes (race tolerated), and existing tests are unaffected.

- [ ] **Step 4: Commit the fix and test conversion**

```bash
git add hogd/auth/auth_server.cc hogd/auth/auth_server_test.cc
git commit -m "fix: tolerate racing update in subscribe_to_auth_data_changes (#1462)

Replace the unconditional CHECK_EQ + bare store with an atomic rmw CAS
that only stores the intro height if the futex is still uninitialized.
Add broadcast() after successful store since a concurrent
register_auth_data_updates may be waiting on the futex.

Convert the death test to a non-death test confirming the race is
tolerated and the cache is queryable afterward."
```
