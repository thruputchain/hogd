# Replay-cache uniquifier consumption in hogexec — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make every transaction included in a block consume its `Uniquifier` into the replay cache (keyed by the root invoked address), in proposal and validation modes, on the success / manual-revert / fault paths; close the related gap where faulting transactions never submit their RCQ observations.

**Architecture:** Wire `tx.uniquifier` through `SystemCallHandler`. On the success path the nonce is reserved as a final subtransaction after the body deltas; the manual-revert and fault paths are unified into a single "discard body, commit per-tx side-records (validation-mode RCQ observations + replay nonce)" path. The nonce reservation is the gating check — a conflict (duplicate/expired/capacity) or a forged observation aborts the whole transaction; everything else commits. `MockInMemory` accepts-and-drops the delta.

**Tech Stack:** C++23, gtest, cmake (`cmake --build ./build -j16`). Spec: `docs/superpowers/specs/2026-05-29-replay-cache-hogexec-integration-design.md`.

Build commands:
- Build a single test target: `cmake --build ./build -j16 --target runtime_test`
- Run a gtest binary filtered: `./build/<path>/<target> --gtest_filter='Suite.Case'`

Key files:
- `hogd/hogexec/runtime/syscall_handler.{h,cc}` — handler, `try_commit_transaction`
- `hogd/hogexec/runtime/invocation_frame.{h,cc}` — `try_acquire_frame_locks`
- `hogd/hogexec/runtime/item/item_view.{h,cc}` — `ReplayCacheItem`
- `hogd/db_interface/db_executor_server.cc` — `DbExecutorRpc::try_reserve_delta` (mock)
- `hogd/hogexec/runtime/native_contract_test_framework.h` — test harness

---

## Task 1: MockInMemory accepts-and-drops ConsumeUniquifier

**Files:**
- Modify: `hogd/db_interface/db_executor_server.cc:20-40` (`DbExecutorRpc::try_reserve_delta`)

- [ ] **Step 1: Confirm the current mock behavior on a ConsumeUniquifier delta**

Run: `git grep -n "try_reserve_storage_delta\|ItemTypeReplayCache\|kItemTypeReplayCache" -- hogd/db_interface hogd/mockdb`
Establish whether `block_state->try_reserve_storage_delta` errors/asserts on a replay-cache delta (the `direct_nonce_tests.cc` skips MockInMemory with "does not support replay cache", so it does not handle it).

- [ ] **Step 2: Drop the delta in the mock path**

In `try_reserve_delta`, after the existing exclusive-lock rejection block (line 30-33), return success without reserving for a consume-uniquifier delta:

```cpp
  // Reject exclusive locks on replay cache items (match HogDB behavior).
  if (delta.type() == StorageDeltaType::kDeltaTypeConsumeUniquifier &&
      (flags & std::to_underlying(StorageDeltaFlags::ExclusiveLock))) {
    return false;
  }
  // MockInMemory does not implement the replay cache (it is being removed).
  // Accept-and-drop consume-uniquifier deltas so transactions that consume a
  // nonce still commit against the mock; replay semantics are verified against
  // HogDB only.
  if (delta.type() == StorageDeltaType::kDeltaTypeConsumeUniquifier) {
    return true;
  }
```

- [ ] **Step 3: Build the affected targets**

Run: `cmake --build ./build -j16 --target db_interface` (or the library target that owns this file; discover via `cmake --build ./build -j16` if unsure)
Expected: compiles clean.

- [ ] **Step 4: Commit**

```bash
git add hogd/db_interface/db_executor_server.cc
git commit -m "mockdb: accept-and-drop ConsumeUniquifier deltas"
```

---

## Task 2: Capture the uniquifier and root address in SystemCallHandler

**Files:**
- Modify: `hogd/hogexec/runtime/syscall_handler.h:372-407` (members)
- Modify: `hogd/hogexec/runtime/syscall_handler.cc:98-134` (Transaction constructor)

- [ ] **Step 1: Add members**

In the private section of `SystemCallHandler` (after `root_invocation_`):

```cpp
  // The transaction's replay-protection uniquifier and the root invoked
  // address it is consumed against. Empty for the test-harness constructor
  // (no Transaction), in which case no nonce is consumed.
  const std::optional<Uniquifier> tx_uniquifier_;
  const std::optional<ContractAddress> root_invoked_address_;
```

- [ ] **Step 2: Populate them in the Transaction constructor**

In the constructor at `syscall_handler.cc:98`, add to the initializer list (alongside `root_invocation_(tx.invocation)`):

```cpp
    tx_uniquifier_(tx.uniquifier),
    root_invoked_address_(tx.invocation.invoked_address),
```

Leave the second (test-harness) constructor's new members default-initialized to `std::nullopt`.

- [ ] **Step 3: Build**

Run: `cmake --build ./build -j16 --target runtime` (the hogexec runtime library target)
Expected: compiles clean (members unused yet — acceptable, or mark used in Task 3).

- [ ] **Step 4: Commit**

```bash
git add hogd/hogexec/runtime/syscall_handler.h hogd/hogexec/runtime/syscall_handler.cc
git commit -m "hogexec: capture tx uniquifier and root address in SystemCallHandler"
```

---

## Task 3: New test file + first failing test (success path burns + dedups)

**Files:**
- Create: `hogd/hogexec/runtime/replay_cache_integration_test.cc`
- Modify: `hogd/hogexec/runtime/CMakeLists.txt` (register the test in `runtime_test_objects`)

The test drives the full path: `make_new_system_call_handler(tx) → root_invoke() → try_commit_transaction()`, on `TestDB::kHogDB`. Use a trivial native contract whose entrypoint just succeeds. Model setup on `syscall_misc_tests.cc` and `direct_nonce_tests.cc`.

- [ ] **Step 1: Write the file with the dedup test**

```cpp
#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <utility>

#include "hogd/hogexec/runtime/native_contract_test_framework.h"
#include "storage_item.hh"
#include "transaction.hh"
#include "types.hh"

namespace hogd {

class ReplayCacheIntegrationTest : public ::testing::Test,
                                   public NativeContractTestFramework {
public:
  ReplayCacheIntegrationTest()
    : NativeContractTestFramework(
          false, BlockExecutionMode::kBlockExecutionModeProposal,
          TestDB::kHogDB)
  {}

  // A trivial native contract whose default entrypoint succeeds.
  // (Replace with the project's canonical "noop"/"echo" native contract;
  //  discover via existing kHogDB tests such as nnint_add_test.cc.)
  void SetUp() override;

  ContractAddress contract_addr_ = make_contract_address(7);
  std::unique_ptr<RuntimeContext> ctx_;
  uint64_t nonce_ = 0;

  Transaction make_tx(uint64_t nonce,
                      BlockHeight expiry = std::numeric_limits<BlockHeight>::max());
  ExecResult run_and_commit(Transaction const &tx);
};
```

Implement `SetUp` to deploy a trivial native contract at `contract_addr_`, grant storage tokens (`setup_grant_storage_token(contract_addr_, 1'000'000'000)`), and build `ctx_ = make_hogexec_context()`. Implement `make_tx` to build a `Transaction` whose `invocation` calls the trivial entrypoint with `gas_limit = UINT64_MAX` and `uniquifier = {expiry, nonce}`. Implement `run_and_commit` as:

```cpp
ExecResult
ReplayCacheIntegrationTest::run_and_commit(Transaction const &tx)
{
  auto handler = make_new_system_call_handler(tx, *ctx_);
  handler.root_invoke();
  return handler.try_commit_transaction();
}
```

First test:

```cpp
TEST_F(ReplayCacheIntegrationTest, DuplicateUniquifierRejected)
{
  auto tx = make_tx(/*nonce=*/100);
  EXPECT_EQ(run_and_commit(tx), ExecResult::ExecResultOK);

  // Same uniquifier again -> replay cache conflict -> not included.
  EXPECT_EQ(run_and_commit(tx), ExecResult::ExecResultTxNonDeterministicError);
}
```

- [ ] **Step 2: Register the test**

In `hogd/hogexec/runtime/CMakeLists.txt`, add `replay_cache_integration_test.cc` to the `runtime_test_objects` source list (alongside `nnint_add_test.cc`).

- [ ] **Step 3: Run — expect FAIL (no consumption yet)**

Run: `cmake --build ./build -j16 --target runtime_test && ./build/hogd/hogexec/runtime/runtime_test --gtest_filter='ReplayCacheIntegrationTest.DuplicateUniquifierRejected'`
Expected: FAIL — the second commit returns `ExecResultOK` because the nonce is never consumed yet.

- [ ] **Step 4: Commit the (red) test**

```bash
git add hogd/hogexec/runtime/replay_cache_integration_test.cc hogd/hogexec/runtime/CMakeLists.txt
git commit -m "test: replay-cache dedup on the hogexec success path (red)"
```

---

## Task 4: Wire the nonce into the success path (A)

**Files:**
- Modify: `hogd/hogexec/runtime/syscall_handler.cc:441-489` (success branch of `try_commit_transaction`)
- Modify: `hogd/hogexec/runtime/syscall_handler.h` (declare the helper)

Reserve the nonce as a **final, separate subtransaction** after the body deltas, before commit. On conflict, `abort_transaction()` discards the whole InvokeTX (body included) — exactly the "not included" outcome.

- [ ] **Step 1: Add a private helper that reserves the nonce on the current batch**

Declare in `syscall_handler.h`:

```cpp
  // Adds the replay-nonce consume delta for the root invoked address to the
  // current subtransaction batch (no-op when there is no uniquifier), then
  // pushes it. Returns the push result: error => system failure;
  // false => reservation conflict (duplicate/expired nonce, or a forged
  // observation already in the batch); true => reserved.
  SystemStatus<bool> reserve_replay_nonce_subtransaction();
```

Define in `syscall_handler.cc`:

```cpp
SystemStatus<bool>
SystemCallHandler::reserve_replay_nonce_subtransaction()
{
  if (tx_uniquifier_.has_value()) {
    assert(root_invoked_address_.has_value());
    db_proxy_.add_delta(
        AddressAndKey{.address = root_invoked_address_.value(),
                      .key = StorageKey(StorageKeyType::ReplayCache)},
        ReplayCacheItem{tx_uniquifier_.value()}.get_delta(),
        std::to_underlying(StorageDeltaFlags::None));
  }
  return db_proxy_.try_push_current_subtransaction();
}
```

Add the include for `ReplayCacheItem` if not already present (`hogd/hogexec/runtime/item/item_view.h`).

- [ ] **Step 2: Call it in the success branch**

In `try_commit_transaction`, after `try_acquire_frame_locks` succeeds (the `bool result = r.value();` block) and before `db_proxy_.commit_transaction(BorrowedContractBatch(...))`, insert the nonce reservation:

```cpp
  // Reserve the replay nonce as the final gating subtransaction. A conflict
  // (duplicate/expired) means the transaction is a replay and is not included.
  {
    auto nonce_res = reserve_replay_nonce_subtransaction();
    if (!nonce_res.has_value()) {
      two_phase_commit_failed_ = true;
      auto abort_res = abort_transaction();
      if (!abort_res.has_value()) {
        return to_exec_result(abort_res.error().code());
      }
      return to_exec_result(nonce_res.error().code());
    }
    if (!nonce_res.value()) {
      two_phase_commit_failed_ = true;
      auto abort_res = abort_transaction();
      if (!abort_res.has_value()) {
        return to_exec_result(abort_res.error().code());
      }
      return ExecResult::ExecResultTxNonDeterministicError;
    }
  }
```

Note: when `tx_uniquifier_` is empty, this pushes an empty subtransaction. Guard the push to only happen when a uniquifier is present OR keep it (an empty push is a harmless no-op savepoint) — verify the mock/HogDB accept an empty batch; if not, wrap the whole block in `if (tx_uniquifier_.has_value())`.

- [ ] **Step 3: Run — expect PASS**

Run: `cmake --build ./build -j16 --target runtime_test && ./build/hogd/hogexec/runtime/runtime_test --gtest_filter='ReplayCacheIntegrationTest.DuplicateUniquifierRejected'`
Expected: PASS.

- [ ] **Step 4: Add expired-height and distinct-nonce tests**

```cpp
TEST_F(ReplayCacheIntegrationTest, DistinctNoncesAccepted)
{
  EXPECT_EQ(run_and_commit(make_tx(1)), ExecResult::ExecResultOK);
  EXPECT_EQ(run_and_commit(make_tx(2)), ExecResult::ExecResultOK);
  EXPECT_EQ(run_and_commit(make_tx(3)), ExecResult::ExecResultOK);
}

TEST_F(ReplayCacheIntegrationTest, ExpiredHeightRejected)
{
  // block height starts at get_block_number(); choose an expiry below it.
  // A uniquifier whose block_expiry_height < current block height is expired.
  auto tx = make_tx(/*nonce=*/9, /*expiry=*/0);
  EXPECT_EQ(run_and_commit(tx), ExecResult::ExecResultTxNonDeterministicError);
}
```

Verify the current block height assumption with `get_block_number()`; if block 0, advance a block first so that `expiry=0` is strictly in the past.

- [ ] **Step 5: Run all three; PASS**

Run: `./build/hogd/hogexec/runtime/runtime_test --gtest_filter='ReplayCacheIntegrationTest.*'`

- [ ] **Step 6: Commit**

```bash
git add hogd/hogexec/runtime/syscall_handler.h hogd/hogexec/runtime/syscall_handler.cc hogd/hogexec/runtime/replay_cache_integration_test.cc
git commit -m "hogexec: consume replay nonce on the tx success path"
```

---

## Task 5: Unify manual-revert and fault paths; nonce + observations on both

**Files:**
- Modify: `hogd/hogexec/runtime/syscall_handler.cc:341-420` (`try_commit_transaction` — the `!is_root_success()` branch and the `should_revert()` branch)
- Modify: `hogd/hogexec/runtime/syscall_handler.h` (declare the shared helper)

- [ ] **Step 1: Add a test that fault/revert burn the nonce (red)**

Add to `replay_cache_integration_test.cc` (needs a native contract entrypoint that manually reverts and one that faults — reuse the project's existing revert/fault native test contracts, as in `manual_revert_*_test.cc` and the fault tests):

```cpp
TEST_F(ReplayCacheIntegrationTest, ManualRevertBurnsNonce)
{
  auto tx = make_revert_tx(/*nonce=*/200);  // entrypoint that manually reverts
  EXPECT_EQ(run_and_commit(tx), ExecResult::ExecResultTxRootFrameReverted);
  // Same uniquifier must now be rejected as a replay.
  EXPECT_EQ(run_and_commit(tx), ExecResult::ExecResultTxNonDeterministicError);
}

TEST_F(ReplayCacheIntegrationTest, FaultBurnsNonce)
{
  auto tx = make_fault_tx(/*nonce=*/300);  // entrypoint that faults
  auto first = run_and_commit(tx);
  EXPECT_EQ(first, ExecResult::ExecResultTxDeterministicError);
  // Same uniquifier must now be rejected as a replay.
  EXPECT_EQ(run_and_commit(tx), ExecResult::ExecResultTxNonDeterministicError);
}
```

Run: expect `ManualRevertBurnsNonce` to FAIL (proposal-mode revert commits empty, no nonce) and `FaultBurnsNonce` to FAIL (fault aborts).

- [ ] **Step 2: Add the shared helper**

Declare in `syscall_handler.h`:

```cpp
  // Commits a transaction whose body effects are discarded (manual revert or
  // deterministic fault). Pushes validation-mode RCQ observation deltas and
  // the replay nonce in one final subtransaction, then commits. A reservation
  // conflict (forged observation or replay) aborts and returns
  // ExecResultTxNonDeterministicError. On success returns success_result.
  ExecResult commit_body_discarded(ExecResult success_result);
```

Define in `syscall_handler.cc` (generalizing the existing manual-revert block at lines 364-420, but sourcing observations from `execution_mode_data_`, not `last_frame_`, since faults have no `last_frame_`):

```cpp
ExecResult
SystemCallHandler::commit_body_discarded(ExecResult success_result)
{
  // Validation mode: resubmit observation deltas so block-close matching can
  // reject forged observations, including on a faulting transaction.
  if (std::holds_alternative<ValidationModeData>(execution_mode_data_)) {
    auto const &observations =
        std::get<ValidationModeData>(execution_mode_data_)
            .observations_to_validate();
    for (auto const &[addrkey, entries] : observations) {
      if (!entries.empty()) {
        StorageDelta delta(StorageDeltaType::kDeltaTypeRetrocausalQueue);
        delta.retrocausal_queue().observed_concurrent_entries = {
            entries.begin(), entries.end()};
        db_proxy_.add_delta(addrkey, std::move(delta),
                            std::to_underlying(StorageDeltaFlags::None));
      }
    }
  }

  auto nonce_res = reserve_replay_nonce_subtransaction();
  if (!nonce_res.has_value()) {
    two_phase_commit_failed_ = true;
    auto abort_status = abort_transaction();
    if (!abort_status.has_value()) {
      return to_exec_result(abort_status.error().code());
    }
    return to_exec_result(nonce_res.error().code());
  }
  if (!nonce_res.value()) {
    two_phase_commit_failed_ = true;
    auto abort_status = abort_transaction();
    if (!abort_status.has_value()) {
      return to_exec_result(abort_status.error().code());
    }
    return ExecResult::ExecResultTxNonDeterministicError;
  }

  auto res = db_proxy_.commit_transaction();
  HogexecMetrics::mark_tx_failed(cumulative_invoke_result_,
                                 get_block_execution_mode());
  if (!res.has_value()) {
    return to_exec_result(res.error().code());
  }
  transaction_committed_or_aborted_ = true;
  return success_result;
}
```

- [ ] **Step 3: Route path B (manual revert) through the helper**

Replace the body of the `if (last_frame_.value().should_revert())` block (lines 364-420) with: extract events as today, then `return commit_body_discarded(ExecResult::ExecResultTxRootFrameReverted);`. Keep `last_frame_.value().extract_events();` before the return so events cannot leak. (The observation-pushing/commit logic now lives in the helper.)

- [ ] **Step 4: Route path C (fault) through the helper for deterministic errors**

In the `if (!cumulative_invoke_result_.is_root_success())` branch (line 345): keep aborting for system/unrecoverable/validation errors, but for deterministic faults commit the side-records. Concretely:

```cpp
  if (!cumulative_invoke_result_.is_root_success()) {
    auto code = cumulative_invoke_result_.result_.error().code();
    if (code == InvokeErrorCode::Unrecoverable) {
      error("try_commit_transaction: received Unrecoverable");
    }
    // Deterministic faults (incl. OutOfGas) are included in the block: discard
    // body effects but still commit RCQ observations + the replay nonce.
    if (code == InvokeErrorCode::Deterministic ||
        code == InvokeErrorCode::OutOfGas) {
      return commit_body_discarded(to_exec_result(code));
    }
    // System / unrecoverable / validation errors: not included.
    auto res = abort_transaction();
    if (!res.has_value()) {
      return to_exec_result(res.error().code());
    }
    return to_exec_result(code);
  }
```

Verify `InvokeErrorCode` member names (`Deterministic`, `OutOfGas`, `Unrecoverable`, `Abandon`, `ValidationFailure`) against `to_exec_result` in `syscall_handler.cc:61-79`.

- [ ] **Step 5: Run the new tests — expect PASS**

Run: `cmake --build ./build -j16 --target runtime_test && ./build/hogd/hogexec/runtime/runtime_test --gtest_filter='ReplayCacheIntegrationTest.*'`
Expected: all PASS.

- [ ] **Step 6: Commit**

```bash
git add hogd/hogexec/runtime/syscall_handler.h hogd/hogexec/runtime/syscall_handler.cc hogd/hogexec/runtime/replay_cache_integration_test.cc
git commit -m "hogexec: burn replay nonce + push RCQ observations on revert and fault"
```

---

## Task 6: Test the RCQ-observation-on-fault gap

**Files:**
- Create: `hogd/hogexec/runtime/anomalies/rcq_observation_on_fault_test.cc`
- Modify: `hogd/hogexec/runtime/anomalies/CMakeLists.txt`

Model on `anomalies/rcq_observation_by_value_not_index_test.cc`: a contract that observes an RCQ entry and then faults. Run in proposal mode, capture `get_proposal_data()`; then in validation mode replay with a **forged** `observed_queue_entry_sequence` and assert the transaction is rejected (commit returns `ExecResultTxNonDeterministicError` / validation failure), and that an honest replay succeeds.

- [ ] **Step 1: Write the test**

Reuse the contract/observation harness from `rcq_observation_by_value_not_index_test.cc`. Key difference: the entrypoint faults after observing. Assert:
- honest validation replay → commit succeeds and block closes (`advance_block(...Validation)` does not throw);
- forged observation in validation → commit returns `ExecResultTxNonDeterministicError` (the observation delta now reaches HogDB on the fault path).

- [ ] **Step 2: Register + run**

Add to `anomalies/CMakeLists.txt` `anomalies_test_objects`. Run:
`cmake --build ./build -j16 --target anomalies_test && ./build/hogd/hogexec/runtime/anomalies/anomalies_test --gtest_filter='*RcqObservationOnFault*'`
Expected: PASS (fails before Task 5 fix; passes after).

- [ ] **Step 3: Commit**

```bash
git add hogd/hogexec/runtime/anomalies/rcq_observation_on_fault_test.cc hogd/hogexec/runtime/anomalies/CMakeLists.txt
git commit -m "test: forged RCQ observation on a faulting tx is rejected in validation"
```

---

## Task 7: Fix existing HogDB-backed tests with proper uniquifiers

**Files:** the kHogDB test files enumerated by failures (see spec; ~40 candidates under `hogd/hogexec/runtime/` and `hogd/hogexec/contract_tests/`).

These tests build `Transaction{...}` with a default `{0,0}` uniquifier and commit more than one against the same invoked address. With the wiring in place, the second commit (and any commit where `block_expiry_height=0 <` current height) now fails. Give each transaction a valid distinct uniquifier following the `syscall_misc_tests.cc` pattern.

- [ ] **Step 1: Build the full hogexec test set and capture failures**

Run: `cmake --build ./build -j16` then run each hogexec test target (or `ctest --test-dir build -R 'runtime_test|anomalies_test|direct_rpc_test|contract'`). Record every failing test.

- [ ] **Step 2: For each failing file, add uniquifiers**

Add a per-fixture `uint64_t nonce_ = 0;` member and set on every `Transaction`:

```cpp
.uniquifier = Uniquifier{.block_expiry_height =
                             std::numeric_limits<BlockHeight>::max(),
                         .nonce = nonce_++},
```

For tests that deliberately submit the *same* logical transaction twice (e.g. idempotency/roundtrip tests), use the **same** uniquifier intentionally and update the expected result if a replay is now correctly rejected — judge per test; do not blanket-bump. Document any such intentional change in the commit message.

- [ ] **Step 3: Rebuild + rerun until green**

Run: `cmake --build ./build -j16 && ctest --test-dir build -R 'runtime_test|anomalies_test|direct_rpc_test'`
Expected: all PASS.

- [ ] **Step 4: Commit (may be several commits, grouped by area)**

```bash
git add -A
git commit -m "test: set valid uniquifiers on hogexec tx tests now that nonces are consumed"
```

---

## Task 8: payments_bench uniquifiers

**Files:**
- Modify: `hogd/hogexec/bench/payments_bench.cc:45-49` and the tx-generation path (`x23`/experiment).

- [ ] **Step 1: Ensure generated work txs carry far-future expiry + unique nonces**

Confirm the ERC20 experiment tx generator sets a unique nonce and a `block_expiry_height` beyond the bench's top block. If not, set them. Leave the existing cleanup TODO (unbounded growth) in place.

- [ ] **Step 2: Build the bench target**

Run: `cmake --build ./build -j16 --target payments_bench` (discover exact target name if different)
Expected: compiles. (Running the bench is optional.)

- [ ] **Step 3: Commit**

```bash
git add hogd/hogexec/bench/payments_bench.cc
git commit -m "bench: give payments_bench txs valid replay uniquifiers"
```

---

## Task 9: Whole-repo build, format, tidy, full test pass

- [ ] **Step 1: Full build**

Run: `cmake --build ./build -j16`
Expected: clean, no `-Werror` failures.

- [ ] **Step 2: clang-format**

Run clang-format on every file touched (the repo enforces it; see prior commits like "fix clang-format"). Use the repo's `.clang-format`.

- [ ] **Step 3: clang-tidy / include-cleaner** on changed files (the repo runs these in CI; add missing direct includes — e.g. `<optional>`, `item_view.h`).

- [ ] **Step 4: Full test run**

Run: `ctest --test-dir build` (or the project's canonical test command)
Expected: all PASS.

- [ ] **Step 5: Commit any format/tidy fixes**

```bash
git add -A
git commit -m "style: clang-format and include-cleaner for replay-cache wiring"
```

---

## Self-review checklist (done while writing)

- **Spec coverage:** goal 1 (consume keyed by root addr, both modes) → Tasks 2,4,5; goal 2 (A/B/C burn, conflict/system skip) → Tasks 4,5; goal 3 (RCQ obs on fault) → Tasks 5,6; goal 4 (mock drop) → Task 1; goal 5 (tests + fixups) → Tasks 3,5,6,7,8. ✓
- **Placeholders:** the trivial/revert/fault native contracts and the exact failing-test set are intentionally discovered against the codebase (Tasks 3,5,7) rather than guessed — each has a concrete discovery command. No "TBD" left in the design itself.
- **Type consistency:** `reserve_replay_nonce_subtransaction()` and `commit_body_discarded(ExecResult)` are used consistently across Tasks 4 and 5; `tx_uniquifier_`/`root_invoked_address_` defined in Task 2 and used in Task 4.
