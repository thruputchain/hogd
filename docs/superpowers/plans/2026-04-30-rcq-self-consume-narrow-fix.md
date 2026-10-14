# RCQ Self-Consume Narrow Fix Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Fix #1604 — a contract triggering Unrecoverable by self-consuming an RCQ entry inserted by a prior TryInvoke child frame.

**Architecture:** Add a `uint16_t max_insert_count_` high-water mark to `RetrocausalQueueIt` so `narrow()` accepts shrinking insert lists. `RetrocausalQueueLock` uses the max for reservation accounting while commit paths use raw delta entries. Early validation in `request_retrocausal_queue` rejects oversized deltas at the RPC boundary.

**Tech Stack:** C++23, GTest, HogDB, hogexec runtime

---

### Task 1: Write failing narrow() unit tests in item_lock_test.cc

**Files:**
- Modify: `hogd/hogdb/item_lock_test.cc`

- [ ] **Step 1: Write test RCQNarrowAcceptsShrinkingInserts**

Append after the `EmptyRCQEntryCommitDoesNotCrash` test (around line 790):

```cpp
TEST(ItemLockTest, RCQNarrowAcceptsShrinkingInserts)
{
  // Sub1 inserts [A, B], sub2 inserts [B] (self-consumed A).
  // narrow() must accept the shrink.
  RetrocausalQueueDelta delta1;
  delta1.insert.added_entries.push_back(QueueEntry{0x0A});
  delta1.insert.added_entries.push_back(QueueEntry{0x0B});
  auto first = ItemChange(RetrocausalQueueIt(std::move(delta1)));

  RetrocausalQueueDelta delta2;
  delta2.insert.added_entries.push_back(QueueEntry{0x0B});
  auto second = ItemChange(RetrocausalQueueIt(std::move(delta2)));

  auto result = second.narrow(first);
  EXPECT_TRUE(result.has_value());
}

TEST(ItemLockTest, RCQNarrowAcceptsShrinkToEmpty)
{
  // Sub1 inserts [A], sub2 inserts nothing (fully self-consumed).
  RetrocausalQueueDelta delta1;
  delta1.insert.added_entries.push_back(QueueEntry{0x0A});
  auto first = ItemChange(RetrocausalQueueIt(std::move(delta1)));

  RetrocausalQueueDelta delta2;
  auto second = ItemChange(RetrocausalQueueIt(std::move(delta2)));

  auto result = second.narrow(first);
  EXPECT_TRUE(result.has_value());
}

TEST(ItemLockTest, RCQNarrowMaxInsertCountTracksAcrossMultipleNarrows)
{
  // Sub1: 3 inserts, Sub2: 1 insert (shrink), Sub3: 4 inserts (grow past peak).
  RetrocausalQueueDelta d1;
  d1.insert.added_entries.push_back(QueueEntry{0x01});
  d1.insert.added_entries.push_back(QueueEntry{0x02});
  d1.insert.added_entries.push_back(QueueEntry{0x03});
  auto c1 = ItemChange(RetrocausalQueueIt(std::move(d1)));

  RetrocausalQueueDelta d2;
  d2.insert.added_entries.push_back(QueueEntry{0x03});
  auto c2 = ItemChange(RetrocausalQueueIt(std::move(d2)));

  auto r1 = c2.narrow(c1);
  ASSERT_TRUE(r1.has_value());

  RetrocausalQueueDelta d3;
  d3.insert.added_entries.push_back(QueueEntry{0x03});
  d3.insert.added_entries.push_back(QueueEntry{0x04});
  d3.insert.added_entries.push_back(QueueEntry{0x05});
  d3.insert.added_entries.push_back(QueueEntry{0x06});
  auto c3 = ItemChange(RetrocausalQueueIt(std::move(d3)));

  auto r2 = c3.narrow(c2);
  ASSERT_TRUE(r2.has_value());
}
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `ctest --test-dir build --output-on-failure --timeout 120 -R "^ItemLockTest\\.RCQNarrow"`

Expected: FAIL — `narrow()` rejects the shrinking insert list with `kInvalidExecutorBehavior`.

- [ ] **Step 3: Commit**

```
git add hogd/hogdb/item_lock_test.cc
git commit -m "test(hogdb): add failing narrow() tests for RCQ self-consume shrink"
```

---

### Task 2: Write failing subtransaction-level tests in direct_rcq_tests.cc

**Files:**
- Modify: `hogd/hogexec/runtime/direct_rpc_tests/direct_rcq_tests.cc`

- [ ] **Step 1: Write test SelfConsumeInsertShrinkAccepted**

Append at end of file (before the closing `} // namespace hogd`):

```cpp
TEST_F(DirectRCQTests, SelfConsumeInsertShrinkAccepted)
{
  setup_grant_storage_token(kContractAddress, 1'000'000'000);
  auto key =
      make_contract_data_addrkey(kContractAddress, "rcq_self_consume_shrink");

  auto tx = make_new_per_tx_db_proxy();

  // Sub1: insert one entry.
  tx.add_delta(key, make_rcq_insert_delta({make_rcq_entry({0xA5})}),
               std::to_underlying(StorageDeltaFlags::None));
  auto r = tx.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && r.value());

  // Sub2: zero inserts (entry was self-consumed by executor).
  tx.add_delta(key, make_rcq_insert_delta({}),
               std::to_underlying(StorageDeltaFlags::None));
  r = tx.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && r.value())
      << "self-consume shrink must not be rejected";

  ASSERT_TRUE(tx.commit_transaction().has_value());
}

TEST_F(DirectRCQTests, SelfConsumePartialShrinkWithNewInserts)
{
  setup_grant_storage_token(kContractAddress, 1'000'000'000);
  auto key =
      make_contract_data_addrkey(kContractAddress, "rcq_partial_self_consume");

  auto tx = make_new_per_tx_db_proxy();

  // Sub1: insert [A, B, C].
  tx.add_delta(
      key,
      make_rcq_insert_delta(
          {make_rcq_entry({0x0A}), make_rcq_entry({0x0B}),
           make_rcq_entry({0x0C})}),
      std::to_underlying(StorageDeltaFlags::None));
  auto r = tx.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && r.value());

  // Sub2: [C, D] — consumed A and B, inserted D.
  tx.add_delta(key,
               make_rcq_insert_delta(
                   {make_rcq_entry({0x0C}), make_rcq_entry({0x0D})}),
               std::to_underlying(StorageDeltaFlags::None));
  r = tx.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && r.value())
      << "partial self-consume with new inserts must succeed";

  ASSERT_TRUE(tx.commit_transaction().has_value());
}

TEST_F(DirectRCQTests, SelfConsumeShrinkThenRollbackRestoresReservation)
{
  setup_grant_storage_token(kContractAddress, 1'000'000'000);
  auto key =
      make_contract_data_addrkey(kContractAddress, "rcq_self_consume_rollback");

  auto tx = make_new_per_tx_db_proxy();

  // Sub1: insert [A].
  tx.add_delta(key, make_rcq_insert_delta({make_rcq_entry({0x0A})}),
               std::to_underlying(StorageDeltaFlags::None));
  auto r = tx.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && r.value());

  // Sub2: zero inserts (self-consumed A).
  tx.add_delta(key, make_rcq_insert_delta({}),
               std::to_underlying(StorageDeltaFlags::None));
  r = tx.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && r.value());

  // Roll back sub2 — restores sub1's state.
  ASSERT_TRUE(tx.pop_subtransactions(1).has_value());

  // Commit — sub1's insert [A] should be intact.
  ASSERT_TRUE(tx.commit_transaction().has_value());
  advance_block();

  auto entry = load_concurrent_entry(key, 0);
  ASSERT_TRUE(entry.has_value());
  EXPECT_EQ(*entry, make_rcq_entry({0x0A}));
}

TEST_F(DirectRCQTests, SelfConsumeReservationPinnedAtPeak)
{
  setup_grant_storage_token(kContractAddress, 1'000'000'000'000);
  auto key = make_contract_data_addrkey(kContractAddress,
                                        "rcq_self_consume_reservation");

  auto tx1 = make_new_per_tx_db_proxy();

  // Sub1: insert 500 entries.
  std::vector<QueueEntry> entries_500;
  entries_500.reserve(500);
  for (int i = 0; i < 500; ++i) {
    entries_500.push_back(make_rcq_entry(
        {static_cast<uint8_t>(i & 0xFF), static_cast<uint8_t>(i >> 8)}));
  }
  tx1.add_delta(key, make_rcq_insert_delta(entries_500),
                std::to_underlying(StorageDeltaFlags::None));
  auto r = tx1.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && r.value());

  // Sub2: zero inserts (all self-consumed).
  tx1.add_delta(key, make_rcq_insert_delta({}),
                std::to_underlying(StorageDeltaFlags::None));
  r = tx1.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && r.value());

  // Concurrent tx2: try to insert 501 entries.
  // Reservation from tx1 is pinned at 500 (the peak), so 500 + 501 > 1000
  // should conflict.
  std::vector<QueueEntry> entries_501;
  entries_501.reserve(501);
  for (int i = 0; i < 501; ++i) {
    entries_501.push_back(
        make_rcq_entry({static_cast<uint8_t>((i + 500) & 0xFF),
                        static_cast<uint8_t>((i + 500) >> 8)}));
  }
  auto tx2 = make_new_per_tx_db_proxy();
  tx2.add_delta(key, make_rcq_insert_delta(entries_501),
                std::to_underlying(StorageDeltaFlags::None));
  r = tx2.try_push_current_subtransaction();
  ASSERT_FALSE(r.has_value())
      << "reservation must be pinned at peak 500; 500 + 501 > 1000 should "
         "conflict";
}

TEST_F(DirectRCQTests, OversizedDeltaRejectedAtRPCBoundary)
{
  setup_grant_storage_token(kContractAddress, 1'000'000'000'000);
  auto key =
      make_contract_data_addrkey(kContractAddress, "rcq_oversized_delta");

  auto tx = make_new_per_tx_db_proxy();

  // Build a delta with kRetrocausalQueueBlockEntriesMaxLen + 1 entries.
  std::vector<QueueEntry> oversized;
  oversized.reserve(kRetrocausalQueueBlockEntriesMaxLen + 1);
  for (size_t i = 0; i <= kRetrocausalQueueBlockEntriesMaxLen; ++i) {
    oversized.push_back(make_rcq_entry(
        {static_cast<uint8_t>(i & 0xFF), static_cast<uint8_t>(i >> 8)}));
  }
  tx.add_delta(key, make_rcq_insert_delta(oversized),
               std::to_underlying(StorageDeltaFlags::None));
  auto r = tx.try_push_current_subtransaction();
  ASSERT_FALSE(r.has_value())
      << "delta with entries > kRetrocausalQueueBlockEntriesMaxLen must be "
         "rejected at the RPC boundary";
}
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `ctest --test-dir build --output-on-failure --timeout 120 -R "^DirectRCQTests\\.SelfConsume"`

Expected: FAIL — `SelfConsumeInsertShrinkAccepted` and `SelfConsumePartialShrinkWithNewInserts` fail because `narrow()` rejects the shrink. `SelfConsumeShrinkThenRollbackRestoresReservation` may also fail at the second push. `SelfConsumeReservationPinnedAtPeak` may fail at the second push or behave differently depending on current reservation behavior.

- [ ] **Step 3: Commit**

```
git add hogd/hogexec/runtime/direct_rpc_tests/direct_rcq_tests.cc
git commit -m "test(direct_rpc): add failing subtransaction tests for RCQ self-consume"
```

---

### Task 3: Write failing full-stack tests in try_invoke_test.cc

**Files:**
- Modify: `hogd/hogexec/runtime/try_invoke_test.cc`

- [ ] **Step 1: Write the PoC test from issue #1604**

Append the test body from the issue before the `INSTANTIATE_TEST_SUITE_P` line (around line 3293). Copy the full test verbatim from the issue body — it defines `kAddressTarget` (method 0 inserts, method 1 consumes) and `kAddressOrchestrator` (TryInvokes method 0 then method 1). The test asserts `Unrecoverable` with the current code.

```cpp
TEST_P(TryInvokeTest, RCQSelfConsumeAfterPriorTryInvokeIsUnrecoverable)
{
  if (GetParam() != TestDB::kHogDB) {
    GTEST_SKIP()
        << "HogDB-only: this exercises subtransaction narrow() behavior";
  }

  static constexpr ContractAddress kAddressTarget = make_contract_address(72);
  static constexpr ContractAddress kAddressOrchestrator =
      make_contract_address(73);

  auto make_context = []() -> std::unique_ptr<RuntimeContext> {
    auto native_contract_target = std::make_unique<NativeContract>(
        std::unordered_map<Runtime::MethodID, NativeContract::function_t>{
            {0,
             [](NativeSyscallContext &ctx) -> InvokeStatus<void> {
               auto key = make_storage_key("rcq-self-consume");
               uint8_t entry = 0xA5;
               auto res = ctx.handler().retrocausal_queue_insert(
                   key, std::as_bytes(std::span(&entry, 1)));
               if (!res.has_value()) {
                 return res.error().to_invoke_error();
               }
               if (!res.value().has_value()) {
                 return InvokeError{InvokeErrorCode::Deterministic};
               }
               return {};
             }},
            {1,
             [](NativeSyscallContext &ctx) -> InvokeStatus<void> {
               auto key = make_storage_key("rcq-self-consume");
               auto res = ctx.handler().retrocausal_queue_consume(key, false);
               if (!res.has_value()) {
                 return res.error().to_invoke_error();
               }
               if (!res.value().has_value()) {
                 return InvokeError{InvokeErrorCode::Deterministic};
               }

               auto const &entry = res.value().value();
               if (entry.size() != 1 || entry[0] != 0xA5) {
                 return InvokeError{InvokeErrorCode::Deterministic};
               }
               return {};
             }},
        });

    auto native_contract_orchestrator = std::make_unique<NativeContract>(
        std::unordered_map<Runtime::MethodID, NativeContract::function_t>{
            {0,
             [](NativeSyscallContext &ctx) -> InvokeStatus<void> {
               auto invoke_target = [&](Runtime::MethodID method) {
                 RuntimeMemoryLength output_return_buffer_len = 0,
                                     output_returned_auth_token_len = 0;
                 return ctx.syscall<SyscallNo::kInvoke>(
                     kAddressTarget, method, ctx.runtime().get_available_gas(),
                     Calldata{}, std::to_underlying(InvokeFlag::TryInvoke),
                     std::span<uint32_t>{}, output_return_buffer_len,
                     output_returned_auth_token_len);
               };

               auto r0 = invoke_target(0);
               if (!r0.has_value() ||
                   r0.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
                 return InvokeError{InvokeErrorCode::Deterministic};
               }

               auto r1 = invoke_target(1);
               if (!r1.has_value()) {
                 if (r1.error().code() == HostFnErrorCode::Unrecoverable) {
                   return InvokeError{InvokeErrorCode::Unrecoverable};
                 }
                 return InvokeError{InvokeErrorCode::Deterministic};
               }
               if (r1.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
                 return InvokeError{InvokeErrorCode::Deterministic};
               }
               return {};
             }},
        });

    std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
    contract_map.emplace(0, std::move(native_contract_target));
    contract_map.emplace(1, std::move(native_contract_orchestrator));
    return std::make_unique<NativeContext>(std::move(contract_map));
  };

  auto context = make_context();

  ContractAddress addrs[] = {kAddressTarget, kAddressOrchestrator};
  for (int i = 0; i < 2; ++i) {
    auto c = make_native_contract(i);
    write_contract(c);
    write_deployment(addrs[i], c);
    setup_grant_storage_token(addrs[i], 1'000'000'000);
  }

  auto handler = make_new_system_call_handler(
      SignedTransaction{
          .tx =
              Transaction{
                  .invocation =
                      Invocation{.invoked_address = kAddressOrchestrator,
                                 .entrypoint = entrypoint_from_method_id(0),
                                 .gas_limit = UINT64_MAX,
                                 .calldata = {}}}},
      *context);

  auto res = handler.root_invoke();

  // After the fix, this should succeed (not Unrecoverable).
  // Before the fix, the test documents the bug: res is Unrecoverable.
  ASSERT_TRUE(res.is_root_success())
      << "RCQ self-consume across TryInvoke frames must not be Unrecoverable";
}
```

Note: The test name matches the issue's PoC but the assertion is flipped — the issue's PoC asserts `Unrecoverable` (documenting the bug), while this version asserts success (the desired behavior after the fix).

- [ ] **Step 2: Write multi-self-consume variant**

Same contract structure, but the orchestrator does three TryInvokes: insert, consume, then insert again. This exercises the grow-shrink-grow path through subtransaction narrowing.

```cpp
TEST_P(TryInvokeTest, RCQMultipleSelfConsumeAndReinsert)
{
  if (GetParam() != TestDB::kHogDB) {
    GTEST_SKIP()
        << "HogDB-only: this exercises subtransaction narrow() behavior";
  }

  static constexpr ContractAddress kAddressTarget = make_contract_address(74);
  static constexpr ContractAddress kAddressOrchestrator =
      make_contract_address(75);

  auto make_context = []() -> std::unique_ptr<RuntimeContext> {
    auto native_contract_target = std::make_unique<NativeContract>(
        std::unordered_map<Runtime::MethodID, NativeContract::function_t>{
            {0,
             [](NativeSyscallContext &ctx) -> InvokeStatus<void> {
               auto key = make_storage_key("rcq-multi");
               uint8_t entry = 0xBB;
               auto res = ctx.handler().retrocausal_queue_insert(
                   key, std::as_bytes(std::span(&entry, 1)));
               if (!res.has_value()) {
                 return res.error().to_invoke_error();
               }
               if (!res.value().has_value()) {
                 return InvokeError{InvokeErrorCode::Deterministic};
               }
               return {};
             }},
            {1,
             [](NativeSyscallContext &ctx) -> InvokeStatus<void> {
               auto key = make_storage_key("rcq-multi");
               auto res = ctx.handler().retrocausal_queue_consume(key, false);
               if (!res.has_value()) {
                 return res.error().to_invoke_error();
               }
               if (!res.value().has_value()) {
                 return InvokeError{InvokeErrorCode::Deterministic};
               }
               return {};
             }},
        });

    auto native_contract_orchestrator = std::make_unique<NativeContract>(
        std::unordered_map<Runtime::MethodID, NativeContract::function_t>{
            {0,
             [](NativeSyscallContext &ctx) -> InvokeStatus<void> {
               auto invoke_target = [&](Runtime::MethodID method) {
                 RuntimeMemoryLength output_return_buffer_len = 0,
                                     output_returned_auth_token_len = 0;
                 return ctx.syscall<SyscallNo::kInvoke>(
                     kAddressTarget, method, ctx.runtime().get_available_gas(),
                     Calldata{}, std::to_underlying(InvokeFlag::TryInvoke),
                     std::span<uint32_t>{}, output_return_buffer_len,
                     output_returned_auth_token_len);
               };

               // insert -> consume -> insert (grow, shrink, grow)
               for (auto method : {0, 1, 0}) {
                 auto r = invoke_target(method);
                 if (!r.has_value()) {
                   return r.error().code() == HostFnErrorCode::Unrecoverable
                              ? InvokeError{InvokeErrorCode::Unrecoverable}
                              : InvokeError{InvokeErrorCode::Deterministic};
                 }
                 if (r.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
                   return InvokeError{InvokeErrorCode::Deterministic};
                 }
               }
               return {};
             }},
        });

    std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
    contract_map.emplace(0, std::move(native_contract_target));
    contract_map.emplace(1, std::move(native_contract_orchestrator));
    return std::make_unique<NativeContext>(std::move(contract_map));
  };

  auto context = make_context();

  ContractAddress addrs[] = {kAddressTarget, kAddressOrchestrator};
  for (int i = 0; i < 2; ++i) {
    auto c = make_native_contract(i);
    write_contract(c);
    write_deployment(addrs[i], c);
    setup_grant_storage_token(addrs[i], 1'000'000'000);
  }

  auto handler = make_new_system_call_handler(
      SignedTransaction{
          .tx =
              Transaction{
                  .invocation =
                      Invocation{.invoked_address = kAddressOrchestrator,
                                 .entrypoint = entrypoint_from_method_id(0),
                                 .gas_limit = UINT64_MAX,
                                 .calldata = {}}}},
      *context);

  auto res = handler.root_invoke();
  ASSERT_TRUE(res.is_root_success())
      << "insert-consume-insert via TryInvoke must succeed";
}
```

- [ ] **Step 3: Run tests to verify they fail**

Run: `ctest --test-dir build --output-on-failure --timeout 120 -R "^TestDBs/TryInvokeTest\\.RCQ"`

Expected: FAIL — both tests return Unrecoverable instead of success.

- [ ] **Step 4: Commit**

```
git add hogd/hogexec/runtime/try_invoke_test.cc
git commit -m "test(runtime): add failing full-stack tests for RCQ self-consume (#1604)"
```

---

### Task 4: Add max_insert_count_ field to RetrocausalQueueIt

**Files:**
- Modify: `hogd/hogdb/item_change.h:465-482`
- Modify: `hogd/hogdb/item_change.cc:98-130`

- [ ] **Step 1: Add field, constructor init, and accessor to item_change.h**

In `hogd/hogdb/item_change.h`, modify the `RetrocausalQueueIt` class:

```cpp
class RetrocausalQueueIt {
public:
  RetrocausalQueueIt(RetrocausalQueueDelta delta)
    : delta_(std::move(delta)),
      max_insert_count_(
          static_cast<uint16_t>(delta_.insert.added_entries.size()))
  {}

  HogDBStatus<bool> narrow(const RetrocausalQueueIt &earlier);

  RetrocausalQueueDelta const &delta() const { return delta_; }

  uint16_t max_insert_count() const { return max_insert_count_; }

private:
  RetrocausalQueueDelta delta_;
  uint16_t max_insert_count_ = 0;
  // Number of entries consumed from past blocks. Calculated during
  // try_atomic_change_lock and used in commit_change.
  mutable uint64_t consumed_past_entries_{0};

  friend struct std::formatter<RetrocausalQueueIt>;
  friend class ExclusiveLock;
  friend class RetrocausalQueueLock;
};
```

- [ ] **Step 2: Fix narrow() in item_change.cc**

Replace the insert-size reject in `RetrocausalQueueIt::narrow()` with the high-water-mark update:

```cpp
HogDBStatus<bool>
RetrocausalQueueIt::narrow(RetrocausalQueueIt const &earlier)
{
  // Track the high-water mark of insert count across all subtransactions.
  // The executor's get_delta() prunes self-consumed entries from the front
  // of the insert list, so the raw size can shrink. The max preserves the
  // peak for reservation accounting in RetrocausalQueueLock.
  max_insert_count_ = std::max(max_insert_count_, earlier.max_insert_count_);

  // The cumulative delta's consume can only grow within a transaction:
  // each frame inherits the prior frame's ItemView state (including consume),
  // and the single-consumer constraint means only one tx per block can consume.
  // An honest executor never drops or reduces consumes.
  if (earlier.delta_.consume && !delta_.consume) {
    return std::unexpected(HogDBError{HogDBCode::kInvalidExecutorBehavior});
  }

  if (earlier.delta_.consume && delta_.consume) {
    auto const &ec = earlier.delta_.consume->past_block_consume;
    auto const &nc = delta_.consume->past_block_consume;
    bool monotonic =
        (ec < nc) ||
        (ec == nc && earlier.delta_.consume->current_block_consumed.size() <=
                         delta_.consume->current_block_consumed.size());
    if (!monotonic) {
      return std::unexpected(HogDBError{HogDBCode::kInvalidExecutorBehavior});
    }
  }
  return true;
}
```

- [ ] **Step 3: Run item_lock_test narrow tests**

Run: `ctest --test-dir build --output-on-failure --timeout 120 -R "^ItemLockTest\\.RCQNarrow"`

Expected: PASS — all three narrow tests from Task 1 now succeed.

- [ ] **Step 4: Commit**

```
git add hogd/hogdb/item_change.h hogd/hogdb/item_change.cc
git commit -m "fix(hogdb): add max_insert_count_ to RetrocausalQueueIt::narrow()"
```

---

### Task 5: Add early validation in request_retrocausal_queue

**Files:**
- Modify: `hogd/hogdb/invoke_tx.cc:469-478`

- [ ] **Step 1: Add size check before constructing RetrocausalQueueIt**

```cpp
HogDBStatus<void>
InvokeTX::request_retrocausal_queue(const ItemKey &key,
                                    const RetrocausalQueueDelta &delta,
                                    StorageDeltaFlagSet flags)
{
  if (delta.insert.added_entries.size() >
      kRetrocausalQueueBlockEntriesMaxLen) {
    return std::unexpected(HogDBError{HogDBCode::kInvalidExecutorBehavior});
  }
  auto change = ItemChange(RetrocausalQueueIt(delta));
  if (flags & std::to_underlying(StorageDeltaFlags::ExclusiveLock)) {
    change.set_permanent_exclusive();
  }
  return request_change(key, std::move(change));
}
```

- [ ] **Step 2: Run existing tests to verify no regression**

Run: `ctest --test-dir build --output-on-failure --timeout 120 -R "^DirectRCQTests\\."`

Expected: All existing DirectRCQTests pass (the new size check is upstream of the existing block-overflow check at the lock layer and doesn't affect valid deltas).

- [ ] **Step 3: Commit**

```
git add hogd/hogdb/invoke_tx.cc
git commit -m "fix(hogdb): validate RCQ insert count at RPC boundary"
```

---

### Task 6: Fix RetrocausalQueueLock reservation accounting and prefix check

**Files:**
- Modify: `hogd/hogdb/item_lock/retrocausal_queue_lock.cc:116-250`

- [ ] **Step 1: Fix unlock path (line 129)**

Change `entries_to_unreserve` to use `max_insert_count()`:

```cpp
  if (!after) {
    // Undo the changes from before.
    CHECK(before != nullptr);
    auto const &delta = before->delta();

    if (delta.consume) {
      consumed_.store(false, std::memory_order_relaxed);
    }

    const size_t entries_to_unreserve = before->max_insert_count();
    if (entries_to_unreserve > 0) {
      reserved_entry_count_.fetch_sub(entries_to_unreserve,
                                      std::memory_order_relaxed);
    }
```

- [ ] **Step 2: Remove the prefix check (lines 148-163)**

Delete the entire block:

```cpp
  // Verify that the shorter insert list is a prefix of the longer one.
  // During normal execution, after's inserts grow (after >= before).
  // During rollback, after's inserts may shrink (after < before).
  if (before != nullptr) {
    auto const &before_entries = before->delta().insert.added_entries;
    auto const &after_entries = delta.insert.added_entries;
    auto const &shorter = (after_entries.size() <= before_entries.size())
                              ? after_entries
                              : before_entries;
    auto const &longer = (after_entries.size() <= before_entries.size())
                             ? before_entries
                             : after_entries;
    if (!std::equal(shorter.begin(), shorter.end(), longer.begin())) {
      return HogDBError{HogDBCode::kInvalidExecutorBehavior};
    }
  }
```

The prefix check is incompatible with self-consume pruning in both the forward path (sub2 after self-consume) and the rollback path (`rollback_to_savepoint` restores entries that aren't a prefix of the pruned list). Safety is covered by `narrow()`, the block overflow check (line 231-233), and the single-consumer constraint.

- [ ] **Step 3: Switch reservation accounting (lines ~213-215 after deletion) to max_insert_count()**

Replace:

```cpp
  const size_t new_entries_count = delta.insert.added_entries.size();
  const size_t previous_entries_count =
      (before != nullptr) ? before->delta().insert.added_entries.size() : 0;
```

With:

```cpp
  const size_t new_entries_count = after->max_insert_count();
  const size_t previous_entries_count =
      (before != nullptr) ? before->max_insert_count() : 0;
```

- [ ] **Step 4: Run all self-consume tests**

Run: `ctest --test-dir build --output-on-failure --timeout 120 -R "DirectRCQTests\\.SelfConsume|ItemLockTest\\.RCQNarrow"`

Expected: PASS — all self-consume tests from Tasks 1 and 2 now succeed.

- [ ] **Step 5: Run the full-stack try_invoke test**

Run: `ctest --test-dir build --output-on-failure --timeout 120 -R "^TestDBs/TryInvokeTest\\.RCQSelfConsumeAfterPriorTryInvoke"`

Expected: PASS — the PoC from issue #1604 now succeeds.

- [ ] **Step 6: Commit**

```
git add hogd/hogdb/item_lock/retrocausal_queue_lock.cc
git commit -m "fix(hogdb): use max_insert_count for RCQ lock reservation accounting"
```

---

### Task 7: Update existing tests affected by prefix check removal

**Files:**
- Modify: `hogd/hogexec/runtime/direct_rpc_tests/direct_rcq_tests.cc:924-944`

- [ ] **Step 1: Update StressSameTxNonCumulativeRCQInserts**

The prefix check that caught non-cumulative inserts of the same size (`{0}` then `{1}`) has been removed. This test's non-cumulative case is no longer detected. Update the test to expect success and document the trade-off:

```cpp
TEST_F(DirectRCQTests, StressSameTxNonCumulativeRCQInserts)
{
  setup_grant_storage_token(kContractAddress, 1'000'000'000);
  auto key = make_contract_data_addrkey(kContractAddress, "rcq_noncumulative");

  auto tx = make_new_per_tx_db_proxy();

  // Subtransaction 1: insert entry {0}.
  tx.add_delta(key, make_rcq_insert_delta({make_rcq_entry({0})}),
               std::to_underlying(StorageDeltaFlags::None));
  auto r = tx.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && r.value());

  // Subtransaction 2: insert entry {1} (NOT cumulative — omits {0}).
  // The prefix check that caught this was removed to support self-consume
  // pruning. Non-cumulative inserts of the same size from a buggy executor
  // are no longer detected at the lock layer. This is accepted: the executor
  // is trusted code, and the entry-level content check was defense-in-depth
  // incompatible with front-pruning semantics.
  tx.add_delta(key, make_rcq_insert_delta({make_rcq_entry({1})}),
               std::to_underlying(StorageDeltaFlags::None));
  r = tx.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && r.value());
}
```

- [ ] **Step 2: Verify all existing direct_rcq_tests pass**

Run: `ctest --test-dir build --output-on-failure --timeout 120 -R "^DirectRCQTests\\."`

Expected: PASS — all tests pass, including `StressSameTxNonCumulativeRCQInserts` with its new expectation.

- [ ] **Step 3: Commit**

```
git add hogd/hogexec/runtime/direct_rpc_tests/direct_rcq_tests.cc
git commit -m "test(direct_rpc): update non-cumulative test for prefix check removal"
```

---

### Task 8: Full test suite verification

- [ ] **Step 1: Run all RCQ-related tests**

Run: `ctest --test-dir build --output-on-failure --timeout 120 -R "RCQ|rcq|RetrocausalQueue|ItemLockTest|DirectRCQTests|TryInvokeTest"`

Expected: All pass.

- [ ] **Step 2: Run the full runtime_test and hogdb_test suites**

Run: `ctest --test-dir build --output-on-failure --timeout 300 -R "runtime_test|hogdb_test"`

Expected: All pass — no regressions.

- [ ] **Step 3: Commit (if any fixups needed)**

Only if fixups were required during verification.
