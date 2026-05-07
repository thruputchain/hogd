# Parallel Batch Decode Failure Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Fix issue #1831 by making metadata-sane but undecodable `ParallelBatch` segments return recoverable parse failure, and by converting scheduler decode failures into non-crashing submit drops or `SchedulerValidationInvalid`.

**Architecture:** Keep `parallel_batch_to_t()` as a decoder with checked top-level metadata preconditions. Add intra-segment bounds and consumption validation so malformed object streams return `false`. Update mempool call sites to handle `false` according to RPC context: submit RPCs return early, validation returns an invalid-block signal.

**Tech Stack:** C++20 coroutines, x23 XDR serialization, GoogleTest, existing `vscode` CMake build directory.

---

## File Structure

- Modify `hogd/base/xdr_util_test.cc`: add minimal `SignedTransaction` test fixture and parser regression tests.
- Modify `hogd/base/xdr_util.h`: document the top-level metadata precondition and add recoverable per-segment decode checks.
- Modify `hogd/mempool/test/mempool_test.cc`: add submit and validation regressions for malformed metadata-sane batches.
- Modify `hogd/mempool/rpc_from_coord.cc`: replace parser-result `CHECK`s with non-crashing behavior in submit and validation paths.

## Task 1: Add Parser Regression Tests

**Files:**
- Modify: `hogd/base/xdr_util_test.cc`
- Test: `vscode/hogd/base/base_test`

- [ ] **Step 1: Add failing parser tests**

Add `<utility>` and `transaction.hh` to the includes in `hogd/base/xdr_util_test.cc`, then add the helper and tests inside `namespace hogd`, before the closing brace:

```cpp
#include <utility>

#include "transaction.hh"
```

```cpp
namespace {

SignedTransaction
make_minimal_signed_transaction()
{
  AuthClaim claim(AuthClaimType::kSourceAddressAuthData);
  claim.source_address_claim().allowed_keys = 0;
  claim.source_address_claim().required_weight = 0;

  return SignedTransaction{
      .tx = Transaction{
          .invocation = Invocation{
              .invoked_address = {},
              .entrypoint = {},
              .gas_limit = 1,
              .calldata = {},
          },
          .uniquifier = {.block_expiry_height = 1, .nonce = 0},
          .contracts_to_deploy = {},
          .auth_claim = std::move(claim),
      },
      .signatures = {},
  };
}

} // namespace

TEST(ParallelBatchToTTest, RoundTripsSingleSignedTransaction)
{
  const SignedTransaction tx = make_minimal_signed_transaction();
  ParallelSignedTransactionBatch batch =
      t_to_parallel_batch(SignedTransactionBatch{tx})();

  SignedTransactionBatch parsed;
  ASSERT_TRUE(parallel_batch_to_t(batch, parsed)());
  ASSERT_EQ(parsed.size(), 1UL);
  EXPECT_EQ(parsed[0], tx);
}

TEST(ParallelBatchToTTest, RejectsOverclaimedFinalSegment)
{
  ParallelSignedTransactionBatch batch = t_to_parallel_batch(
      SignedTransactionBatch{make_minimal_signed_transaction()})();

  ASSERT_EQ(batch.segment_tokens.size(), 1UL);
  ASSERT_EQ(batch.segment_tokens[0], 1U);
  ASSERT_EQ(batch.segment_bytes[0], batch.xdr.size());
  batch.segment_tokens[0] = 2;

  SignedTransactionBatch parsed;
  EXPECT_FALSE(parallel_batch_to_t(batch, parsed)());
}

TEST(ParallelBatchToTTest, RejectsLeftoverSegmentBytes)
{
  ParallelSignedTransactionBatch batch = t_to_parallel_batch(
      SignedTransactionBatch{make_minimal_signed_transaction()})();

  ASSERT_EQ(batch.segment_tokens.size(), 1UL);
  ASSERT_EQ(batch.segment_tokens[0], 1U);
  batch.xdr.resize(batch.xdr.size() + 4, 0);
  batch.segment_bytes[0] += 4;

  SignedTransactionBatch parsed;
  EXPECT_FALSE(parallel_batch_to_t(batch, parsed)());
}
```

- [ ] **Step 2: Build the parser test binary**

Run:

```bash
cmake --build vscode --target base_test -j8
```

Expected: build succeeds. If the new helper has a compile error, fix only the test code and rerun this step.

- [ ] **Step 3: Verify the parser tests fail before implementation**

Run:

```bash
vscode/hogd/base/base_test --gtest_filter='ParallelBatchToTTest.*'
```

Expected: nonzero exit. `RejectsLeftoverSegmentBytes` should fail because the current decoder returns `true` with leftover segment bytes. In debug builds, `RejectsOverclaimedFinalSegment` may abort on the current one-past-end `x23assert`; that is also a valid red result.

## Task 2: Harden `parallel_batch_to_t()`

**Files:**
- Modify: `hogd/base/xdr_util.h`
- Test: `vscode/hogd/base/base_test`

- [ ] **Step 1: Document the decoder precondition**

Add this comment immediately before `template<typename T>` for `parallel_batch_to_t()`:

```cpp
// Decodes a ParallelBatch whose top-level segment metadata has already been
// validated by the caller. A mismatch between [segment_tokens] and
// [segment_bytes], or between the sum of [segment_bytes] and [xdr.size()], is a
// caller bug and remains a CHECK failure. Malformed object XDR inside otherwise
// sane segment metadata is untrusted input and is reported by returning false.
```

- [ ] **Step 2: Add recoverable intra-segment checks**

Replace the inner segment decode loop in `parallel_batch_to_t()` with:

```cpp
        size_t inner_obj_offset = 0;
        size_t inner_xdr_offset = 0;
        for (size_t j = 0; j < num_segment_tokens; j++) {
          if (inner_xdr_offset >= num_segment_bytes) {
            info("failed to deserialize: segment {} claims {} tokens but "
                 "byte offset {} reached segment size {}",
                 i, num_segment_tokens, inner_xdr_offset, num_segment_bytes);
            co_return false;
          }

          try {
            const size_t obj_idx = obj_offsets[i] + inner_obj_offset;
            size_t num_consumed = x23::deserialize_from(
                &objs[obj_idx],
                x23::BufView(&parallel.xdr[byte_offsets[i] + inner_xdr_offset],
                             num_segment_bytes - inner_xdr_offset));
            inner_obj_offset++;
            inner_xdr_offset += num_consumed;
            if (inner_xdr_offset > num_segment_bytes) {
              info("failed to deserialize: segment {} consumed {} bytes, "
                   "exceeding segment size {}",
                   i, inner_xdr_offset, num_segment_bytes);
              co_return false;
            }
          } catch (const x23::XdrBadInput &e) {
            info("failed to deserialize: {}", e.what());
            co_return false;
          }
        }

        if (inner_xdr_offset != num_segment_bytes) {
          info("failed to deserialize: segment {} left {} trailing bytes", i,
               num_segment_bytes - inner_xdr_offset);
          co_return false;
        }
```

- [ ] **Step 3: Verify parser tests pass**

Run:

```bash
cmake --build vscode --target base_test -j8
vscode/hogd/base/base_test --gtest_filter='ParallelBatchToTTest.*'
```

Expected: build succeeds and all `ParallelBatchToTTest.*` tests pass.

- [ ] **Step 4: Commit parser hardening**

Run:

```bash
git add hogd/base/xdr_util.h hogd/base/xdr_util_test.cc
git commit -m "fix(base): reject malformed parallel batch segments"
```

## Task 3: Add Submit-Path Regression Tests

**Files:**
- Modify: `hogd/mempool/test/mempool_test.cc`
- Test: `vscode/hogd/mempool/test/mempool_test`

- [ ] **Step 1: Add failing submit tests**

Add these tests after `TEST(Mempool, SubmitVetted)`:

```cpp
TEST(Mempool, SubmitUnvettedMalformedBatchDoesNotCrash)
{
  fib_client_t fib_client(MempoolTestInfra::kAccountAddresses[0]);
  const SignedTransactionBatch txs = generate_unsigned_txs(fib_client, 1);
  ParallelSignedTransactionBatch batch =
      t_to_parallel_batch(SignedTransactionBatch{txs[0]})();

  ASSERT_EQ(batch.segment_tokens.size(), 1UL);
  batch.segment_tokens[0]++;

  MempoolTestInfra server;
  server.mempool_coord_rpc().schedulercoord_submit_unvetted(batch)();
  EXPECT_EQ(server.mempool_coord_rpc().scheduler()->size(), 0UL);
}

TEST(Mempool, SubmitVettedMalformedBatchDoesNotCrash)
{
  fib_client_t fib_client(MempoolTestInfra::kAccountAddresses[0]);
  const SignedTransactionBatch txs = generate_unsigned_txs(fib_client, 1);
  ParallelSignedTransactionBatch batch =
      t_to_parallel_batch(SignedTransactionBatch{txs[0]})();

  ASSERT_EQ(batch.segment_tokens.size(), 1UL);
  batch.segment_tokens[0]++;

  MempoolTestInfra server;
  ValidatorID validator_id{};
  server.mempool_coord_rpc().schedulercoord_submit_vetted(validator_id,
                                                          batch)();
  EXPECT_EQ(server.mempool_coord_rpc().scheduler()->size(), 0UL);
}
```

- [ ] **Step 2: Build the mempool test binary**

Run:

```bash
cmake --build vscode --target mempool_test -j8
```

Expected: build succeeds. If the new test has a compile error, fix only the test code and rerun this step.

- [ ] **Step 3: Verify submit tests fail before call-site handling**

Run:

```bash
vscode/hogd/mempool/test/mempool_test --gtest_filter='Mempool.Submit*MalformedBatchDoesNotCrash'
```

Expected: nonzero exit because the current submit paths still `CHECK` the parser result.

## Task 4: Handle Submit Decode Failures Without Crashing

**Files:**
- Modify: `hogd/mempool/rpc_from_coord.cc`
- Test: `vscode/hogd/mempool/test/mempool_test`

- [ ] **Step 1: Update unvetted submit parse handling**

Replace:

```cpp
  auto txs = std::make_shared<SignedTransactionBatch>();
  CHECK(co_await parallel_batch_to_t(batch, *txs));
```

with:

```cpp
  auto txs = std::make_shared<SignedTransactionBatch>();
  if (!(co_await parallel_batch_to_t(batch, *txs))) {
    info("submit_unvetted rejected malformed transaction batch");
    co_return;
  }
```

- [ ] **Step 2: Update vetted submit parse handling**

Replace:

```cpp
  SignedTransactionBatch txs;
  CHECK(co_await parallel_batch_to_t(batch, txs));
```

with:

```cpp
  SignedTransactionBatch txs;
  if (!(co_await parallel_batch_to_t(batch, txs))) {
    info("submit_vetted rejected malformed transaction batch");
    co_return;
  }
```

- [ ] **Step 3: Verify submit tests pass**

Run:

```bash
cmake --build vscode --target mempool_test -j8
vscode/hogd/mempool/test/mempool_test --gtest_filter='Mempool.Submit*MalformedBatchDoesNotCrash'
```

Expected: build succeeds and both submit malformed-batch tests pass.

- [ ] **Step 4: Commit submit handling**

Run:

```bash
git add hogd/mempool/rpc_from_coord.cc hogd/mempool/test/mempool_test.cc
git commit -m "fix(mempool): drop malformed submitted batches"
```

## Task 5: Add Validation Regression Test

**Files:**
- Modify: `hogd/mempool/test/mempool_test.cc`
- Test: `vscode/hogd/mempool/test/mempool_test`

- [ ] **Step 1: Add failing validation test**

Add this test after `TEST(Mempool, Validate)`:

```cpp
TEST(Mempool, ValidateMalformedNominationDecodeReturnsInvalid)
{
  fib_client_t fib_client(MempoolTestInfra::kAccountAddresses[0]);
  const SignedTransactionBatch txs = generate_unsigned_txs(fib_client, kNumTxs);

  MempoolTestInfra server;

  ValidatorID validator_id{};
  server.mempool_coord_rpc().schedulercoord_submit_vetted(
      validator_id, t_to_parallel_batch(SignedTransactionBatch{txs[0]})())();

  SchedulerGetTipResult get_tip =
      server.mempool_coord_rpc().schedcoord_get_tip()();
  EXPECT_EQ(get_tip.outcome(), SchedulerGetTipOutcome::SchedulerGetTipOK);
  DbTip db_tip = get_tip.db_tip();
  auto res_nominate =
      server.mempool_coord_rpc().schedulercoord_nominate(db_tip)();
  ASSERT_EQ(res_nominate.outcome(),
            SchedulerNominationOutcome::SchedulerNominationOK);

  auto res_commit = server.mempool_coord_rpc().schedulercoord_commit_block(
      res_nominate.nominated_block().block_id)();
  EXPECT_EQ(res_commit, SchedulerCommitOutcome::SchedulerCommitOK);

  Nomination nomination = res_nominate.nominated_block().nomination;
  nomination.block_height++;
  nomination.prev_block_hash = nomination.block_hash;
  {
    SignedTransactionHashBatch tx_hashes;
    EXPECT_TRUE(parallel_batch_to_t(nomination.transactions_hash, tx_hashes)());

    get_tip = server.mempool_coord_rpc().schedcoord_get_tip()();
    EXPECT_EQ(get_tip.outcome(), SchedulerGetTipOutcome::SchedulerGetTipOK);
    db_tip = get_tip.db_tip();
    EXPECT_EQ(db_tip.height, 1);

    nomination.block_hash =
        sha256(BlockHeader{.network_id = MempoolTestInfra::kNetworkId,
                           .block_height = nomination.block_height,
                           .txset_hash =
                               MempoolCoordinatorRpc::get_txset_hash(tx_hashes),
                           .db_hash = db_tip.db_hash,
                           .events_hash = db_tip.events_hash,
                           .prev_block_hash = nomination.prev_block_hash});
  }

  ASSERT_FALSE(nomination.transactions_hash.segment_tokens.empty());
  ASSERT_FALSE(
      nomination.transactions_nondeterministic_data.segment_tokens.empty());
  nomination.transactions_hash.segment_tokens[0]++;
  nomination.transactions_nondeterministic_data.segment_tokens[0]++;

  server.mempool_coord_rpc().schedulercoord_submit_vetted(
      validator_id, t_to_parallel_batch(SignedTransactionBatch{txs[0]})())();
  auto res_validate =
      server.mempool_coord_rpc().schedulercoord_validate(db_tip, nomination)();
  EXPECT_EQ(res_validate.outcome(),
            SchedulerValidationResult::SchedulerValidationInvalid);
}
```

- [ ] **Step 2: Build the mempool test binary**

Run:

```bash
cmake --build vscode --target mempool_test -j8
```

Expected: build succeeds. If the new test has a compile error, fix only the test code and rerun this step.

- [ ] **Step 3: Verify validation test fails before validation handling**

Run:

```bash
vscode/hogd/mempool/test/mempool_test --gtest_filter='Mempool.ValidateMalformedNominationDecodeReturnsInvalid'
```

Expected: nonzero exit because `schedulercoord_validate()` still `CHECK`s the parser result.

## Task 6: Return Invalid on Validation Decode Failure

**Files:**
- Modify: `hogd/mempool/rpc_from_coord.cc`
- Test: `vscode/hogd/mempool/test/mempool_test`

- [ ] **Step 1: Replace validation parse `CHECK`s**

In `MempoolCoordinatorRpc::schedulercoord_validate()`, replace this block:

```cpp
    CHECK(co_await tx_nondeterministic_data_handle);
    record_stage(stage_deserialize_nomination_data);

    cache.block_id = generate_block_id();
    validation_telemetry.block_id = cache.block_id;
    CHECK(co_await tx_hash_handle);
    validation_telemetry.tx_count = cache.tx_hashes.size();
    record_stage(stage_deserialize_tx_hashes);
```

with:

```cpp
    const bool decoded_tx_nominations =
        co_await tx_nondeterministic_data_handle;
    record_stage(stage_deserialize_nomination_data);

    const bool decoded_tx_hashes = co_await tx_hash_handle;
    record_stage(stage_deserialize_tx_hashes);

    if (!decoded_tx_nominations || !decoded_tx_hashes) {
      co_await decrement_num_validations_in_progress();
      validation_telemetry.outcome = validation_outcome_str(
          SchedulerValidationOutcome::SchedulerValidationInvalid);
      info("schedulercoord_validate rejected malformed nomination batch");
      co_return SchedulerValidationResult(
          SchedulerValidationOutcome::SchedulerValidationInvalid);
    }

    cache.block_id = generate_block_id();
    validation_telemetry.block_id = cache.block_id;
    validation_telemetry.tx_count = cache.tx_hashes.size();
```

This awaits both parse handles before returning, preserving the existing no-abandoned-coroutine behavior.

- [ ] **Step 2: Verify validation test passes**

Run:

```bash
cmake --build vscode --target mempool_test -j8
vscode/hogd/mempool/test/mempool_test --gtest_filter='Mempool.ValidateMalformedNominationDecodeReturnsInvalid'
```

Expected: build succeeds and the validation malformed-nomination test passes.

- [ ] **Step 3: Commit validation handling**

Run:

```bash
git add hogd/mempool/rpc_from_coord.cc hogd/mempool/test/mempool_test.cc
git commit -m "fix(mempool): invalidate undecodable nominations"
```

## Task 7: Final Verification

**Files:**
- Verify: `hogd/base/xdr_util.h`
- Verify: `hogd/base/xdr_util_test.cc`
- Verify: `hogd/mempool/rpc_from_coord.cc`
- Verify: `hogd/mempool/test/mempool_test.cc`

- [ ] **Step 1: Build targeted tests**

Run:

```bash
cmake --build vscode --target base_test mempool_test -j8
```

Expected: build succeeds.

- [ ] **Step 2: Run targeted parser and mempool regressions**

Run:

```bash
vscode/hogd/base/base_test --gtest_filter='ParallelBatchToTTest.*'
vscode/hogd/mempool/test/mempool_test --gtest_filter='Mempool.Submit*MalformedBatchDoesNotCrash:Mempool.ValidateMalformedNominationDecodeReturnsInvalid'
```

Expected: all targeted tests pass.

- [ ] **Step 3: Run existing nearby tests**

Run:

```bash
vscode/hogd/base/base_test --gtest_filter='Int128XdrBytesTest.*:ParallelBatchToTTest.*'
vscode/hogd/mempool/test/mempool_test --gtest_filter='Mempool.SubmitUnvetted:Mempool.SubmitVetted:Mempool.Validate:Mempool.ValidateBatch:Mempool.ValidateMalformedNominationDecodeReturnsInvalid'
```

Expected: all listed tests pass.

- [ ] **Step 4: Inspect final diff**

Run:

```bash
git diff --stat HEAD~3..HEAD
git status --short
```

Expected: only the planned files are changed by the implementation commits. Pre-existing unrelated dirty files may still appear in `git status --short`; do not stage or revert them.
