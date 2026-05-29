# Replay-cache uniquifier consumption in the hogexec tx execution path

Date: 2026-05-29
Status: Design — pending review

## Problem

A `Transaction` already carries a `Uniquifier { BlockHeight block_expiry_height;
uint64_t nonce; }` (see `xdr/transaction.hh`). Its purpose is replay protection:
the pair `(invoked address, block_expiry_height, nonce)` must be globally unique,
and the transaction becomes invalid after `block_expiry_height`.

All of the downstream machinery to enforce this already exists:

- `ReplayCacheItem` (`hogexec/runtime/item/item_view.{h,cc}`) produces a
  `ConsumeUniquifier` storage delta.
- `ReplayCacheLock` (`hogdb/item_lock/replay_cache_lock.cc`) enforces, on the
  HogDB side, that a consumed uniquifier is not expired
  (`block_expiry_height >= block_height`), not a duplicate (in-flight set + base
  state), and within capacity (`kReplayCacheNoncesMaxLen`, `kReplayCacheHeightsMaxLen`).

But the wiring is missing: `SystemCallHandler` extracts `tx.invocation`,
`tx.auth_claim`, and `tx.contracts_to_deploy`, and **ignores `tx.uniquifier`**.
`ReplayCacheItem` is never instantiated outside its own definition. The replay
cache is exercised only by direct tests that hand-build `ConsumeUniquifier`
deltas (`direct_rpc_tests/direct_nonce_tests.cc`,
`anomalies/replay_cache_height_budget_leak_test.cc`). The author anticipated this
work: `syscall_misc_tests.cc` already sets `block_expiry_height = max(), nonce =
nonce_++` on its transactions, and `bench/payments_bench.cc` carries a TODO that
"this benchmark will break down when we start tracking uniquifiers within hogdb."

## Goals

1. Every transaction that is **included in the block** consumes its uniquifier
   (the "replay nonce") into the replay cache, keyed by the **root invoked
   address**, in both proposal and validation modes.
2. "Included" means the transaction's outcome is one of:
   - **A. Success** (`is_root_success() && !should_revert()`)
   - **B. Manual revert** (`is_root_success() && should_revert()`)
   - **C. Deterministic fault / out-of-gas** (`!is_root_success()` with a
     deterministic error code)

   In all three the replay entry is committed. The **only** outcomes that do not
   burn the nonce are:
   - a replay-cache **conflict** on the nonce reservation itself
     (duplicate / expired / capacity) — the transaction is then rejected and not
     included, and
   - a **system / unrecoverable / validation** error (transport failure, stale
     block, forged proposal data) — the transaction is not included.
3. Close a pre-existing gap surfaced by this work: path **C** must also push its
   **RCQ observation deltas** in validation mode. Today it calls
   `abort_transaction()`, so a transaction that observes a retrocausal-queue
   entry and then faults never submits its observations, and block-close matching
   cannot reject a forged observation on a faulting transaction.
4. `MockInMemory` accepts-and-drops `ConsumeUniquifier` deltas (no replay
   semantics). It is being removed and is broken in other ways; correctness is
   verified only against HogDB.
5. Tests: new `NativeContractTestFramework` tests asserting duplicates rejected
   and heights respected; a new test for the RCQ-observation-on-fault gap; and
   uniquifier fixups for existing HogDB-backed tests that would otherwise start
   colliding.

## Non-goals

- No protocol-version or config gate — the wiring is **unconditional** (the XDR
  field already exists and is part of the transaction).
- No change to gas / fee accounting for failed transactions.
- No `MockInMemory` replay-cache support (it is being removed).
- No change to *which* transactions the scheduler/coordinator chooses to include;
  this is purely about what the executor commits for an included transaction.

## Background: how a transaction is finalized today

`SystemCallHandler::try_commit_transaction()` (`runtime/syscall_handler.cc`) has
three terminal paths. RCQ observation deltas are pushed only in **validation**
mode — `ProposalModeData::observations_to_validate()` returns empty, because the
proposer generated the observations and has nothing to validate.

| Path | Condition | Proposal mode | Validation mode |
|------|-----------|---------------|-----------------|
| A. Success | `is_root_success() && !should_revert()` | `try_acquire_frame_locks` pushes body deltas, commits | body deltas + observation deltas, commits |
| B. Manual revert | `is_root_success() && should_revert()` | commits an **empty** transaction | pushes **observation deltas only**, commits |
| C. Fault / OOG | `!is_root_success()` | `abort_transaction()` | `abort_transaction()` |

Mechanics that make the design clean:

- The proxy is a savepoint stack: `add_delta` → `try_push_current_subtransaction`
  (reserves, returns `false` on conflict) → `pop_subtransactions` /
  `commit_transaction` / `abort_transaction`.
- Child `try_invoke` frames push their own savepoints during execution; a regular
  call merges into its parent's `modified_items_` and pushes nothing.
- `mark_frame_as_failed` (`invocation_frame.cc:101`) pops a frame's
  `try_invoke_frame_count_` savepoints, and it runs during unwinding whenever a
  frame **reverts or faults** (`finish_frame`, line 372). Therefore, by the time
  a root fault reaches `try_commit_transaction`, the body's savepoints are
  already popped — **paths B and C arrive at commit with an empty savepoint
  stack**. They differ only in their condition and returned `ExecResult`.
- A hard fault (path C) never invokes `last_frame_action_`, so `last_frame_` is
  **not set**. Observations on path C must come from
  `execution_mode_data_.observations_to_validate()`, and the nonce from the
  stored `tx.uniquifier` — not from the frame.

## Design

### Capture the uniquifier and root address

`SystemCallHandler` stores `tx.uniquifier` and the root invoked address (it
already moves `tx.invocation` into `root_invocation_`; capture the
`invoked_address` and uniquifier before the move, or read them where needed).
The replay-cache key is `make_replay_cache_addrkey(root_invoked_address)`, and
the delta is the existing `ReplayCacheItem{uniquifier}.get_delta()` /
`ConsumeUniquifierArgs{uniquifier}`.

### Path A (success): nonce rides in the root-commit batch

The nonce delta must be added to the single subtransaction that
`try_acquire_frame_locks` pushes — but **only at the root-commit call site**, not
on the child `try_invoke` pushes (`invocation_frame.cc:313`, which passes
`no_obs`). Otherwise a transaction with N successful try_invokes would consume
its nonce N+1 times.

Approach: give `try_acquire_frame_locks` an additional optional parameter (e.g.
`std::optional<Uniquifier> root_uniquifier`, default `nullopt`). The root-commit
call in `try_commit_transaction` passes the transaction's uniquifier; the child
call passes `nullopt`. When present, the method adds the `ConsumeUniquifier`
delta (keyed by the root invoked address) to the batch before
`try_push_current_subtransaction`. A `false` result (conflict) propagates exactly
as a body-delta conflict does today → abort, non-deterministic error, nonce not
burned.

### Paths B and C: one shared "commit side-records only" helper

Generalize the existing manual-revert block into a helper used by both B and C —
e.g. `commit_with_body_discarded(ExecResult success_result)` — that:

1. (validation mode only) pushes the RCQ observation deltas from
   `observations_to_validate()` — the existing B logic.
2. (both modes) adds the `ConsumeUniquifier` delta for the root invoked address
   to the same batch.
3. `try_push_current_subtransaction()`:
   - transport / system error → mark two-phase-commit failed, `abort_transaction`,
     return the system `ExecResult` (not included, no burn);
   - `false` (conflict — replay or forged observation) → `abort_transaction`,
     return `ExecResultTxNonDeterministicError` (not included, no burn);
   - `true` → `commit_transaction()`, return `success_result`.

Path B calls it with `ExecResultTxRootFrameReverted`; path C calls it with the
mapped deterministic `ExecResult` (`to_exec_result(error_code)`), replacing the
current `abort_transaction()`. Path C must guard for the `last_frame_`-absent
case (no events to extract; observations and nonce come from handler state).

Note: when the validation-mode observation batch and the nonce are pushed in one
subtransaction, an observation mismatch and a replay conflict both surface as the
same `false` → `ExecResultTxNonDeterministicError`, which is the correct
"reject, not included" outcome for both.

### MockInMemory: accept-and-drop

In `db_interface/db_executor_server.cc` (`DbExecutorRpc::try_reserve_delta`),
accept a `kDeltaTypeConsumeUniquifier` delta as a no-op (reserve nothing, return
`true`), preserving the existing exclusive-lock rejection. This lets the ~40
HogDB-or-mock test files that commit transactions keep working against the mock
without replay semantics. HogDB behavior is unchanged.

### Determinism / validation

The nonce delta is a pure function of `tx.uniquifier`, so validation reproduces
the identical delta; nonce reservation matches across propose/validate the same
way observation deltas do. Path C's new observation push closes the forged-
observation gap for faulting transactions.

## Test plan

All new correctness tests use `TestDB::kHogDB`.

1. **New file** `hogd/hogexec/runtime/replay_cache_integration_test.cc`
   (`NativeContractTestFramework`, full `make_new_system_call_handler` →
   `root_invoke` → `try_commit_transaction` path; a trivial native contract):
   - `DuplicateUniquifierRejected` — two transactions to the same address with the
     same uniquifier: first commits, second is rejected
     (`ExecResultTxNonDeterministicError`), and the first's effect persists.
   - `DistinctNoncesAccepted` — several transactions, distinct nonces, all commit;
     replay-cache metadata reflects the consumed nonces.
   - `ExpiredHeightRejected` — a uniquifier with `block_expiry_height <` current
     block height is rejected.
   - `SuccessRevertFaultAllBurn` — a successful, a manually-reverted, and a
     faulting transaction each burn their nonce: resubmitting the same uniquifier
     afterward is rejected in every case.
   - A basic heights/pruning assertion across an `advance_block`.
2. **RCQ-observation-on-fault gap** — a test (alongside
   `anomalies/rcq_observation_by_value_not_index_test.cc`) where a transaction
   observes a retrocausal-queue entry and then faults: capture proposal data, then
   replay in validation mode with a **forged** observation and assert it is
   rejected (block-close matching fails). Confirms path C now submits observation
   deltas.
3. **Existing HogDB tests** — wherever a test commits more than one transaction
   against the same invoked address (or commits a transaction with a default
   `{0,0}` uniquifier, which is both a duplicate and immediately expired), give
   each transaction a valid distinct uniquifier following the established
   `syscall_misc_tests.cc` pattern: `block_expiry_height =
   std::numeric_limits<BlockHeight>::max()`, `nonce = nonce_++`. The exact set is
   enumerated during implementation by building and running each HogDB test target.

## Risks and edge cases

- **Double-consume via try_invoke** — mitigated by gating the nonce on the
  root-commit call to `try_acquire_frame_locks`.
- **`last_frame_` absent on fault** — the shared helper sources observations and
  the nonce from handler/mode state, not the frame.
- **Existing tests with default uniquifiers** — will now collide on HogDB; fixed
  by item 3 of the test plan. Mock-backed tests are unaffected (delta dropped).
- **`payments_bench.cc`** — its TODO becomes live (uniquifiers grow unbounded).
  Out of scope for correctness; give bench transactions far-future expiry and
  unique nonces so it still runs, and leave the cleanup TODO.
- **Capacity limits** — `kReplayCacheNoncesMaxLen` / `kReplayCacheHeightsMaxLen`
  are enforced by `ReplayCacheLock`; no new handling needed in hogexec.
