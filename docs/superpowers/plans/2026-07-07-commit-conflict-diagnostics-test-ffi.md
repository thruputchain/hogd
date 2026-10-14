# Surface 2-phase-commit conflict diagnostics through test_ffi — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Surface `DbConflictResult` two-phase-commit conflict diagnostics through test_ffi — the tx-level conflict as a new field on the `try_commit_transaction` FFI result, and each failing `try_invoke` sub-frame's conflict inside its invocation trace.

**Architecture:** The conflict is already captured as a `DbConflictResult` in `db_proxy_.last_push_conflict()`. Two channels surface it: (1) a `SystemCallHandler::last_commit_conflict()` getter read by the FFI `try_commit_transaction` and serialized into a new `serialized_conflict` buffer; (2) a new `commit_conflict` field on the XDR `InvocationFrameTrace`, populated for a failing `try_invoke` frame, which rides out on the existing `serialized_trace`. Both always emit the whole `DbConflictResult` union (including the `kDbDiagnosticNone` arm) so consumers always deserialize a well-formed, self-describing value.

**Tech Stack:** C++23, x23 XDR (`.x` → generated `.hh`), GoogleTest, CMake, Rust bindgen (auto-run by `build.rs`).

## Global Constraints

- **Build:** ONLY `cmake --build build …` (the existing **Debug** dir; `kDbDiagnosticAll` requires a debug build) or `cmake --build build_release …`. NEVER run `build.sh`, `cmake` configure, or any reconfigure. NEVER run two builds concurrently.
- **Branch:** Work on `geoff/commit-conflict-diagnostics-test-ffi` (already checked out; hogd requires a `geoff/*` prefix).
- **Staging:** Stage explicit paths only. NEVER `git add -A`/`-a` (`build_tsan/` etc. are not gitignored and get swept).
- **Includes:** CI tidy enforces `misc-include-cleaner` as an error — directly `#include` the canonical provider of every type you name (here: `db_conflict.hh` for `DbConflictResult`/`DbConflictCode`/`DbDiagnosticLevel`).
- **No compat shims:** one signature per function; use a defaulted out-param (the codebase idiom, cf. `try_push_new_subtransaction(..., conflict_out = nullptr)`), not an overload.
- **groundhog-go:** the `transaction_trace.x` protocol change's downstream resync (groundhog-go / protocol-go) is handled separately by the maintainer — OUT OF SCOPE for this plan.

---

### Task 1: Add `commit_conflict` field to the `InvocationFrameTrace` XDR type

**Files:**
- Modify: `protocol/transaction_trace.x`

**Interfaces:**
- Produces: `InvocationFrameTrace` gains a value field `DbConflictResult commit_conflict;` (default-constructs to the `kDbDiagnosticNone` arm). Consumed by Task 2 (population) and Task 4 (FFI/tests).

- [ ] **Step 1: Add the `db_conflict.hh` include to the `.x` header block**

In `protocol/transaction_trace.x`, add the include alongside the existing `%#include` lines at the top (after `%#include "event.hh"`):

```
%#include "invoke_args.hh"
%#include "db_conflict.hh"
```

- [ ] **Step 2: Add the field to `InvocationFrameTrace`**

Find the `struct InvocationFrameTrace` definition near the end of the file and add `commit_conflict` as the last field:

```
struct InvocationFrameTrace {
  Invocation invocation;

  /// The ordered list of system calls made by this frame.
  SyscallTrace system_calls<>;

  /// The set of storage writes made by this frame,
  /// when the frame terminates.
  /// Note that this field may be nonempty even if the frame Faults or Aborts;
  /// In such a case, these writes will not be applied to the parent frame,
  /// but are still reported in the trace for testing purposes.
  FlaggedStorageDelta storage_writes<>;

  /// Status of the frame when it terminates.
  InvocationFrameOutput status;

  /// Two-phase-commit conflict diagnostic for a try_invoke frame whose
  /// subtransaction push failed (proposal mode). The kDbDiagnosticNone arm on
  /// every other frame. Best-effort SDK-testing diagnostic; see
  /// SystemCallHandler::finish_frame.
  DbConflictResult commit_conflict;
};
```

- [ ] **Step 3: Build to regenerate the header and confirm it compiles**

Run: `cmake --build build --target runtime_test -j16`
Expected: SUCCESS. The build regenerates `build/hogd/xdr/transaction_trace.hh` with the new field; all existing `InvocationFrameTrace` construction sites keep compiling because the field default-initializes. (Roundtrip coverage in `serialize_roundtrip_test.cc` is serialize→deserialize→equal, so the appended field is compatible.)

- [ ] **Step 4: Commit**

```bash
git add protocol/transaction_trace.x
git commit -m "protocol: add commit_conflict to InvocationFrameTrace"
```

---

### Task 2: Populate `commit_conflict` on a failing `try_invoke` frame's trace

**Files:**
- Modify: `hogd/hogexec/runtime/invocation_frame.h` (add include + out-param)
- Modify: `hogd/hogexec/runtime/invocation_frame.cc:280-368` (set the out-param on the proposal try_invoke branch)
- Modify: `hogd/hogexec/runtime/syscall_handler.cc:904-967` (reorder + write the child trace)
- Test: `hogd/hogexec/runtime/try_invoke_test.cc` (extend `TryInvokeOnExclusiveLock`)

**Interfaces:**
- Consumes: `InvocationFrameTrace::commit_conflict` (Task 1); `DatabaseProxy::last_push_conflict()` returning `DbConflictResult const&`.
- Produces: `InvocationFrameStack::finish_frame(InvokeStatus<void>, ExecutionModeData&, DbConflictResult *try_invoke_conflict_out = nullptr)` — sets `*try_invoke_conflict_out` to the frame's post-acquire `last_push_conflict()` on the proposal-mode try_invoke branch (`None` on success).

- [ ] **Step 1: Write the failing test — extend `TryInvokeOnExclusiveLock`**

In `hogd/hogexec/runtime/try_invoke_test.cc`, in `TEST_P(TryInvokeTest, TryInvokeOnExclusiveLock)`: enable tracing on `handler2` before its `root_invoke()`, and after the `commit_res2` assertion add trace assertions.

Add `handler2.enable_tracing();` immediately before the existing `auto res2 = handler2.root_invoke();` line.

Then, immediately after the existing `ASSERT_TRUE(commit_res2 == ExecResult::ExecResultTxDeterministicError);`, insert:

```cpp
  // The failing try_invoke child frame's trace carries the 2pc conflict.
  auto const *trace2 = handler2.get_trace();
  ASSERT_NE(trace2, nullptr);
  SyscallTrace const *invoke_sc = nullptr;
  for (auto const &sc : trace2->system_calls) {
    if (sc.body.syscall_number() == SyscallNo::kInvoke) {
      invoke_sc = &sc;
      break;
    }
  }
  ASSERT_NE(invoke_sc, nullptr);
  auto const &invoke_trace = invoke_sc->body.invoke();
  ASSERT_TRUE(invoke_trace.outputs.has_value());
  ASSERT_NE(invoke_trace.outputs->child_trace, nullptr);
  auto const &child_trace = *invoke_trace.outputs->child_trace;
  // Debug build => full vector of conflicts (kDbDiagnosticAll).
  ASSERT_EQ(child_trace.commit_conflict.level(),
            DbDiagnosticLevel::kDbDiagnosticAll);
  bool found_excl = false;
  for (auto const &c : child_trace.commit_conflict.conflicts()) {
    if (c.code == DbConflictCode::kDbConflictExclusiveLock) {
      found_excl = true;
    }
  }
  ASSERT_TRUE(found_excl);
```

Add `#include "db_conflict.hh"` to the include block of `try_invoke_test.cc` if not already present.

- [ ] **Step 2: Run the test to verify it fails**

Run: `cmake --build build --target runtime_test -j16 && ./build/bin/runtime_test --gtest_filter='*TryInvokeOnExclusiveLock*'`
Expected: BUILD SUCCESS, test FAILS — `commit_conflict.level()` is `kDbDiagnosticNone` (default), so the `kDbDiagnosticAll` assertion fails. (This confirms the field exists but isn't populated yet.)

- [ ] **Step 3: Add the include and out-param to `InvocationFrameStack::finish_frame`**

In `hogd/hogexec/runtime/invocation_frame.h`, add the canonical include near the other project includes:

```cpp
#include "db_conflict.hh"
```

Change the `finish_frame` declaration (currently at `invocation_frame.h:311-313`) to add the defaulted out-param:

```cpp
  // Returns a pair of the InvokeResult and a bool indicating whether the frame
  // was manually reverted.
  // If try_invoke_conflict_out is non-null and this frame is a proposal-mode
  // try_invoke, it is set to the frame's post-acquire two-phase-commit conflict
  // (kDbDiagnosticNone on a successful acquire).
  SystemStatus<std::pair<InvokeResult, bool>>
  finish_frame(InvokeStatus<void> result, ExecutionModeData &execution_mode_data,
               DbConflictResult *try_invoke_conflict_out = nullptr);
```

- [ ] **Step 4: Set the out-param in the proposal try_invoke branch**

In `hogd/hogexec/runtime/invocation_frame.cc`, update the `finish_frame` definition signature to match the header:

```cpp
InvocationFrameStack::finish_frame(InvokeStatus<void> result,
                                   ExecutionModeData &execution_mode_data,
                                   DbConflictResult *try_invoke_conflict_out)
```

Then, in the proposal-mode branch, immediately after `acq_success = acq_res.value();` (currently `invocation_frame.cc:318`) and before the `log_nondeterministic_bit(acq_success)` call, capture the conflict:

```cpp
        acq_success = acq_res.value();

        // Hand the just-attempted push's conflict diagnostic back to the caller
        // so it can be recorded on this frame's trace. None on a successful
        // acquire (last_push_conflict_ is reset at the start of every push).
        if (try_invoke_conflict_out) {
          *try_invoke_conflict_out = db_proxy_.last_push_conflict();
        }

        std::get<ProposalModeData>(execution_mode_data)
            .log_nondeterministic_bit(acq_success);
```

- [ ] **Step 5: Reorder `SystemCallHandler::finish_frame` and write the child trace**

In `hogd/hogexec/runtime/syscall_handler.cc`, replace the body of `SystemCallHandler::finish_frame` from the parent-restore block through the stack-finish call (currently `syscall_handler.cc:930-966`) so the stack `finish_frame` runs first, the conflict is written onto the still-current child trace, and only then the parent trace is restored and the child attached.

Replace this existing region:

```cpp
  // Restore parent trace and attach child
  if (stashed_parent_trace) {
    auto child_trace = std::move(invocation_frame_trace_);
    invocation_frame_trace_ = std::move(stashed_parent_trace);
    // child_trace can be null when start_frame() dropped this frame's trace
    // after an allocation failure (#2473); in that case there is nothing to
    // attach and the parent's Invoke entry simply records no child trace.
    if (child_trace) {
      // Attach child_trace to parent's last syscall (the Invoke entry)
      assert(!invocation_frame_trace_->system_calls.empty());
      auto &invoke_body = invocation_frame_trace_->system_calls.back().body;
      CHECK(invoke_body.syscall_number() == SyscallNo::kInvoke);
      try {
        auto &out = invoke_body.invoke();
        out.outputs.emplace(SyscallInvokeOutputs());
        out.outputs->child_trace = std::move(*child_trace);
      } catch (std::exception &e) {
        // Child-trace attachment is best-effort diagnostics. finish_frame() is
        // noexcept and reachable from the test FFI mock-frame path, so a
        // std::bad_alloc while materializing the optional SyscallInvokeOutputs
        // must drop the optional trace rather than panic()-terminate an honest
        // caller (#2741), mirroring start_frame()'s drop-and-continue (#2473).
        // A failed emplace() leaves out.outputs empty (no partial attachment),
        // and the no-allocation reporter avoids throwing again from this catch.
        report_dropped_log(e.what());
      }
    }
  }

  [[maybe_unused]] size_t stack_size = invocation_frame_stack_.size();
  auto res = invocation_frame_stack_.finish_frame(result, execution_mode_data_);
  size_t stack_size_after = invocation_frame_stack_.size();
  assert(stack_size_after == stack_size - 1);
  if (stack_size_after == 0) {
    last_frame_finished_ = true;
  }
  return res;
```

with:

```cpp
  // Finish the frame on the stack first, capturing any try_invoke two-phase-
  // commit conflict. This must run after log_trace_finish() above (which reads
  // the finishing frame) and before the parent-trace restore below, so the
  // conflict can be recorded on this (still-current) child frame's trace.
  [[maybe_unused]] size_t stack_size = invocation_frame_stack_.size();
  DbConflictResult try_invoke_conflict{DbDiagnosticLevel::kDbDiagnosticNone};
  auto res = invocation_frame_stack_.finish_frame(result, execution_mode_data_,
                                                  &try_invoke_conflict);
  size_t stack_size_after = invocation_frame_stack_.size();
  assert(stack_size_after == stack_size - 1);
  if (stack_size_after == 0) {
    last_frame_finished_ = true;
  }

  // Record the conflict on this frame's trace. Best-effort: copying the All arm
  // allocates, and finish_frame() is noexcept and reachable from the test FFI
  // mock path, so a std::bad_alloc drops the diagnostic rather than terminating.
  if (invocation_frame_trace_ &&
      try_invoke_conflict.level() != DbDiagnosticLevel::kDbDiagnosticNone) {
    try {
      invocation_frame_trace_->commit_conflict = std::move(try_invoke_conflict);
    } catch (std::exception &e) {
      report_dropped_log(e.what());
    }
  }

  // Restore parent trace and attach child
  if (stashed_parent_trace) {
    auto child_trace = std::move(invocation_frame_trace_);
    invocation_frame_trace_ = std::move(stashed_parent_trace);
    // child_trace can be null when start_frame() dropped this frame's trace
    // after an allocation failure (#2473); in that case there is nothing to
    // attach and the parent's Invoke entry simply records no child trace.
    if (child_trace) {
      // Attach child_trace to parent's last syscall (the Invoke entry)
      assert(!invocation_frame_trace_->system_calls.empty());
      auto &invoke_body = invocation_frame_trace_->system_calls.back().body;
      CHECK(invoke_body.syscall_number() == SyscallNo::kInvoke);
      try {
        auto &out = invoke_body.invoke();
        out.outputs.emplace(SyscallInvokeOutputs());
        out.outputs->child_trace = std::move(*child_trace);
      } catch (std::exception &e) {
        // Child-trace attachment is best-effort diagnostics. finish_frame() is
        // noexcept and reachable from the test FFI mock-frame path, so a
        // std::bad_alloc while materializing the optional SyscallInvokeOutputs
        // must drop the optional trace rather than panic()-terminate an honest
        // caller (#2741), mirroring start_frame()'s drop-and-continue (#2473).
        // A failed emplace() leaves out.outputs empty (no partial attachment),
        // and the no-allocation reporter avoids throwing again from this catch.
        report_dropped_log(e.what());
      }
    }
  }

  return res;
```

(`syscall_handler.cc` already `#include`s `db_conflict.hh` at line 23, so `DbConflictResult`/`DbDiagnosticLevel` are in scope.)

- [ ] **Step 6: Run the test to verify it passes**

Run: `cmake --build build --target runtime_test -j16 && ./build/bin/runtime_test --gtest_filter='*TryInvokeOnExclusiveLock*'`
Expected: PASS.

- [ ] **Step 7: Run the full try_invoke + invocation_frame suites for regressions**

Run: `./build/bin/runtime_test --gtest_filter='*TryInvoke*:*InvocationFrame*:*Tracing*'`
Expected: PASS (no regression from the reorder or the signature change).

- [ ] **Step 8: Commit**

```bash
git add hogd/hogexec/runtime/invocation_frame.h hogd/hogexec/runtime/invocation_frame.cc hogd/hogexec/runtime/syscall_handler.cc hogd/hogexec/runtime/try_invoke_test.cc
git commit -m "hogexec: record try_invoke 2pc conflict on the frame trace"
```

---

### Task 3: `SystemCallHandler::last_commit_conflict()` getter

**Files:**
- Modify: `hogd/hogexec/runtime/syscall_handler.h` (add include + declaration)
- Modify: `hogd/hogexec/runtime/syscall_handler.cc` (add definition)
- Test: `hogd/hogexec/runtime/nnint_add_test.cc` (extend the root-conflict case in `SetAddWithAdd`)

**Interfaces:**
- Produces: `DbConflictResult const &SystemCallHandler::last_commit_conflict() const` — returns `db_proxy_.last_push_conflict()`; valid after `try_commit_transaction()` returns (abort does not reset it). Consumed by Task 4 (FFI).

- [ ] **Step 1: Write the failing test — assert the root conflict getter in `SetAddWithAdd`**

In `hogd/hogexec/runtime/nnint_add_test.cc`, in `TEST_P(NNIntAddTest, SetAddWithAdd)`, immediately after the existing `ASSERT_EQ(commit_res4, ExecResult::ExecResultTxNonDeterministicError);` (currently line 388), insert:

```cpp
  // The getter surfaces the same diagnostic the FFI serializes out. handler4's
  // add drives the nnint below zero => bounds-exceeded conflict.
  ASSERT_EQ(handler4.last_commit_conflict().level(),
            DbDiagnosticLevel::kDbDiagnosticAll);
  bool found_bounds = false;
  for (auto const &c : handler4.last_commit_conflict().conflicts()) {
    if (c.code == DbConflictCode::kDbConflictBoundsExceeded) {
      found_bounds = true;
    }
  }
  ASSERT_TRUE(found_bounds);
```

Add `#include "db_conflict.hh"` to the include block of `nnint_add_test.cc` if not already present.

- [ ] **Step 2: Run the test to verify it fails**

Run: `cmake --build build --target runtime_test -j16`
Expected: BUILD FAILS — `last_commit_conflict` is not a member of `SystemCallHandler` yet. (This is the red state.)

- [ ] **Step 3: Declare the getter**

In `hogd/hogexec/runtime/syscall_handler.h`, add the canonical include near the other project includes:

```cpp
#include "db_conflict.hh"
```

Then add the declaration just after the `try_commit_transaction()` declaration (currently `syscall_handler.h:194`):

```cpp
  ExecResult try_commit_transaction();

  // The two-phase-commit conflict diagnostic from the most recent failed commit
  // push (the root lock-acquire / 2pc push, or the commit_body_discarded RCQ /
  // replay path), at the DbDiagnosticLevel the database selected from the block
  // mode. The kDbDiagnosticNone arm if the last push succeeded. Valid after
  // try_commit_transaction() returns: abort_transaction() does not reset it.
  // Intended for SDK / test surfacing.
  DbConflictResult const &last_commit_conflict() const;
```

- [ ] **Step 4: Define the getter**

In `hogd/hogexec/runtime/syscall_handler.cc`, add the definition next to `get_trace()` (near `syscall_handler.cc:1044`):

```cpp
DbConflictResult const &
SystemCallHandler::last_commit_conflict() const
{
  return db_proxy_.last_push_conflict();
}
```

- [ ] **Step 5: Run the test to verify it passes**

Run: `cmake --build build --target runtime_test -j16 && ./build/bin/runtime_test --gtest_filter='*NNIntAddTest.SetAddWithAdd*'`
Expected: PASS.

- [ ] **Step 6: Commit**

```bash
git add hogd/hogexec/runtime/syscall_handler.h hogd/hogexec/runtime/syscall_handler.cc hogd/hogexec/runtime/nnint_add_test.cc
git commit -m "hogexec: add SystemCallHandler::last_commit_conflict getter"
```

---

### Task 4: FFI `serialized_conflict` on `try_commit_transaction`

**Files:**
- Modify: `hogd/test_ffi/test_bindings.h` (struct field + version bump)
- Modify: `hogd/test_ffi/formatter.h` (`ForeignTryCommitResult` formatter)
- Modify: `hogd/test_ffi/test_bindings.cc` (serialize in the FFI `try_commit_transaction`)
- Test: `hogd/test_ffi/test_bindings_test.cc` (new test driving a commit conflict through the FFI)

**Interfaces:**
- Consumes: `SystemCallHandler::last_commit_conflict()` (Task 3); `serialize_to_ffi_data_buffer` (`ffi_data_buffer.h`); `mock_contract_create_event` (existing success mock in `test_bindings_test.cc`).
- Produces: `struct ForeignTryCommitResult { int32_t result_code; struct FFIDataBuffer serialized_conflict; enum TestFFIErrorCode ffi_error_code; }` — `serialized_conflict` is always the XDR-serialized full `DbConflictResult` (owned; free with `free_ffi_data_buffer`), null only on a serialization failure.

- [ ] **Step 1: Write the failing test — drive a replay conflict through the FFI**

In `hogd/test_ffi/test_bindings_test.cc`, add `#include "db_conflict.hh"` to the include block, and add this test near the other `try_commit_transaction` tests:

```cpp
TEST(TestBindingsTest, TryCommitSurfacesReplayConflictDiagnostic)
{
  auto *db = make_db();
  ASSERT_NE(db, nullptr);
  x23::Defer d([db] { free_db(db); });

  auto *rc = make_runtime_context();
  ASSERT_NE(rc, nullptr);
  x23::Defer d2([rc] { close_runtime_context(rc); });

  ASSERT_EQ(register_new_contract(rc, 1, mock_contract_create_event, nullptr),
            kTestFFIErrorCodeNone);

  Contract contract(ContractType::kMockTest);
  contract.mock_contract_id() = 1;
  auto bytes = x23::serialize(contract);
  ASSERT_EQ(setup_create_contract(rc, db, bytes.data(), bytes.size()),
            kTestFFIErrorCodeNone);

  ContractAddress deployment_addr{};
  ASSERT_EQ(setup_deploy_contract(db, deployment_addr.data(),
                                  sha256(contract).data(), bytes.size()),
            kTestFFIErrorCodeNone);
  ASSERT_EQ(setup_grant_contract_storage_tokens(db, deployment_addr.data(),
                                                10'000'000),
            kTestFFIErrorCodeNone);

  auto make_tx = [&] {
    return SignedTransaction{
        .tx = {.invocation = {.invoked_address = deployment_addr,
                              .entrypoint = entrypoint_from_method_id(0x1234),
                              .gas_limit = 1'000'000,
                              .calldata = {}},
               .uniquifier = {.block_expiry_height = INT64_MAX, .nonce = 0}},
        .signatures = {}};
  };

  // First tx commits OK, consuming the replay nonce for this block.
  {
    auto stx = make_tx();
    auto *h = make_syscall_handler(db, rc, stx);
    x23::Defer dh([h] { close_syscall_handler(h); });
    auto r = root_invoke(h, 0);
    ASSERT_EQ(r.ffi_error_code, kTestFFIErrorCodeNone);
    free_ffi_data_buffer(r.serialized_output);
    free_ffi_data_buffer(r.serialized_trace);
    free_ffi_data_buffer(r.serialized_events);
    auto c = try_commit_transaction(h);
    ASSERT_EQ(c.ffi_error_code, kTestFFIErrorCodeNone);
    ASSERT_EQ(c.result_code, std::to_underlying(ExecResult::ExecResultOK));
    free_ffi_data_buffer(c.serialized_conflict);
  }

  // Second tx, same uniquifier -> replay conflict at commit.
  auto stx2 = make_tx();
  auto *h2 = make_syscall_handler(db, rc, stx2);
  x23::Defer dh2([h2] { close_syscall_handler(h2); });
  auto r2 = root_invoke(h2, 0);
  ASSERT_EQ(r2.ffi_error_code, kTestFFIErrorCodeNone);
  free_ffi_data_buffer(r2.serialized_output);
  free_ffi_data_buffer(r2.serialized_trace);
  free_ffi_data_buffer(r2.serialized_events);

  auto c2 = try_commit_transaction(h2);
  ASSERT_EQ(c2.ffi_error_code, kTestFFIErrorCodeNone);
  ASSERT_EQ(c2.result_code,
            std::to_underlying(ExecResult::ExecResultTxNonDeterministicError));
  ASSERT_NE(c2.serialized_conflict.data, nullptr);
  x23::Defer dc([&] { free_ffi_data_buffer(c2.serialized_conflict); });

  DbConflictResult conflict;
  x23::deserialize(&conflict, x23::BufView{c2.serialized_conflict.data,
                                           c2.serialized_conflict.len});
  ASSERT_EQ(conflict.level(), DbDiagnosticLevel::kDbDiagnosticAll);
  bool found_replay = false;
  for (auto const &c : conflict.conflicts()) {
    if (c.code == DbConflictCode::kDbConflictReplayProtection) {
      found_replay = true;
    }
  }
  ASSERT_TRUE(found_replay);
}
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `cmake --build build --target test_ffi_test -j16`
Expected: BUILD FAILS — `ForeignTryCommitResult` has no member `serialized_conflict`. (Red state.)

- [ ] **Step 3: Add the `serialized_conflict` field and bump the version**

In `hogd/test_ffi/test_bindings.h`, bump the version (the struct layout change is ABI-breaking):

```c
#define TEST_FFI_VERSION_MAJOR 0
#define TEST_FFI_VERSION_MINOR 20
#define TEST_FFI_VERSION_PATCH 0
```

And add the field to `ForeignTryCommitResult`, between `result_code` and `ffi_error_code`:

```c
// Translation of try_commit_transaction result into an FFI-safe type.
struct ForeignTryCommitResult {
  int32_t result_code; // ExecResult

  // XDR-serialized DbConflictResult two-phase-commit conflict diagnostic. Always
  // populated with the whole union (the kDbDiagnosticNone arm when there was no
  // conflict) so the caller always deserializes a well-formed, self-describing
  // value. Null only if serialization itself failed. Must be freed with
  // free_ffi_data_buffer().
  struct FFIDataBuffer serialized_conflict;

  // Errors that arise from the FFI bindings, and NOT from the regular operation
  // of hogexec.
  enum TestFFIErrorCode ffi_error_code;
};
```

- [ ] **Step 4: Update the `ForeignTryCommitResult` formatter**

In `hogd/test_ffi/formatter.h`, update the `formatter<ForeignTryCommitResult>::format` body:

```cpp
    return format_to(ctx.out(),
                     "result_code={}, serialized_conflict=[{}], "
                     "ffi_error_code={}",
                     result.result_code, result.serialized_conflict,
                     std::to_underlying(result.ffi_error_code));
```

- [ ] **Step 5: Serialize the conflict in the FFI `try_commit_transaction`**

In `hogd/test_ffi/test_bindings.cc`, in the internal `hogd::try_commit_transaction` (the success path around `test_bindings.cc:839-852`), replace:

```cpp
    auto res = context->to_handler().try_commit_transaction();
    context->mark_committed_or_aborted();
    if (res == ExecResult::ExecResultOK) {
      auto events_res =
          context->to_handler().emit_events(ExecResult::ExecResultOK,
                                            ProposedTransaction{})();
      if (!events_res.has_value()) {
        error("failed to emit events: {}", events_res.error());
        return ForeignTryCommitResult{
            .result_code = std::to_underlying(
                SystemCallHandler::to_exec_result(events_res.error().code()))};
      }
    }
    return ForeignTryCommitResult{.result_code = std::to_underlying(res)};
```

with:

```cpp
    auto res = context->to_handler().try_commit_transaction();
    context->mark_committed_or_aborted();

    // Always serialize the full DbConflictResult (including the None arm) so the
    // caller gets a well-formed, self-describing value: a wrong discriminant or
    // a format drift is visible rather than masquerading as "no conflict". Best-
    // effort: a serialization failure degrades to a null buffer (reserved for
    // "serialization failed", distinct from the None arm) rather than turning a
    // real commit result into an FFI error.
    FFIDataBuffer serialized_conflict = kNullBuffer;
    {
      auto s = serialize_to_ffi_data_buffer(
          context->to_handler().last_commit_conflict());
      if (s.has_value()) {
        serialized_conflict = s.value();
      } else {
        best_effort_log([] {
          error("try_commit_transaction: failed to serialize conflict "
                "diagnostic");
        });
      }
    }

    if (res == ExecResult::ExecResultOK) {
      auto events_res =
          context->to_handler().emit_events(ExecResult::ExecResultOK,
                                            ProposedTransaction{})();
      if (!events_res.has_value()) {
        error("failed to emit events: {}", events_res.error());
        return ForeignTryCommitResult{
            .result_code = std::to_underlying(
                SystemCallHandler::to_exec_result(events_res.error().code())),
            .serialized_conflict = serialized_conflict};
      }
    }
    return ForeignTryCommitResult{.result_code = std::to_underlying(res),
                                  .serialized_conflict = serialized_conflict};
```

(`test_bindings.cc` already includes `ffi_data_buffer.h`/`db_conflict.hh` transitively via the handler headers; if the build reports `DbConflictResult` or `serialize_to_ffi_data_buffer` unresolved, add `#include "db_conflict.hh"` and confirm `#include "hogd/test_ffi/ffi_data_buffer.h"` are present.)

- [ ] **Step 6: Run the test to verify it passes**

Run: `cmake --build build --target test_ffi_test -j16 && ./build/bin/test_ffi_test --gtest_filter='*TryCommitSurfacesReplayConflictDiagnostic*'`
Expected: PASS.

- [ ] **Step 7: Run the full test_ffi suite for regressions**

Run: `./build/bin/test_ffi_test`
Expected: PASS (existing `try_commit_transaction` callers see the extra field default-zeroed via `make_ffi_result_type`, which is fine).

- [ ] **Step 8: Verify the Rust bindings regenerate against the new header**

Run: `cd hogd/rust/test_ffi_bindings && cargo build 2>&1 | tail -20`
Expected: SUCCESS. `build.rs` re-runs bindgen against `test_bindings.h`, regenerating `bindings.rs` (in `OUT_DIR`, not committed) with the new `serialized_conflict` field and version constants.

- [ ] **Step 9: Commit**

```bash
git add hogd/test_ffi/test_bindings.h hogd/test_ffi/formatter.h hogd/test_ffi/test_bindings.cc hogd/test_ffi/test_bindings_test.cc
git commit -m "test_ffi: return commit conflict diagnostic from try_commit_transaction"
```

---

## Coverage boundary (intentional)

The **try_invoke-conflict-through-the-FFI-trace** path is covered transitively, not by a bespoke end-to-end FFI test:

- Task 2 proves `commit_conflict` is populated on a failing try_invoke child frame (via `handler.get_trace()`).
- The FFI `serialized_trace` is produced by `serialize_to_ffi_data_buffer(*get_trace())`, and existing `test_bindings_test.cc` tests (e.g. the nested `child_trace` roundtrip tests around lines 2173-2199 / 2693-2712) prove the whole nested `InvocationFrameTrace` tree serializes and deserializes faithfully — so the appended `commit_conflict` field rides out with it.

A dedicated FFI test driving a *real* two-contract exclusive-lock try_invoke conflict (the FFI mock-frame path does **not** run the real `try_acquire_frame_locks`, so it would require a full real-syscall two-contract fixture) is deliberately out of scope here. Add one later if belt-and-suspenders end-to-end coverage is wanted.

## Self-review notes

- **Spec coverage:** Part A (getter → Task 3; FFI field + serialize-whole-union → Task 4). Part B (XDR field → Task 1; out-param + reorder population → Task 2; serialization = none, rides `get_trace()` → noted). Part C (version bump + bindings → Task 4). All covered.
- **Type consistency:** `last_commit_conflict()`, `finish_frame(..., DbConflictResult *try_invoke_conflict_out = nullptr)`, `commit_conflict`, `serialized_conflict` used consistently across tasks. `DbConflictResult` accessors (`.level()`, `.conflicts()`) and enums (`DbDiagnosticLevel::kDbDiagnosticAll`, `DbConflictCode::kDbConflict*`) match the generated `db_conflict.hh`.
- **Debug requirement:** all conflict assertions expect `kDbDiagnosticAll`, which requires the Debug `build/` dir — consistent with the Global Constraints.
