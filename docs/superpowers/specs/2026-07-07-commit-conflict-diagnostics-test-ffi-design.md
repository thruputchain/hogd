# Surface two-phase-commit conflict diagnostics through test_ffi

## Problem

`SystemCallHandler::try_commit_transaction()` (and the try_invoke sub-frame
commit path) already capture *why* a two-phase commit failed as a
`DbConflictResult` (`protocol/db_conflict.x`): a `DbConflictCode` per
conflicting storage op, at the `DbDiagnosticLevel` the database selects from the
block's execution mode. In a **debug** build a proposal captures
`kDbDiagnosticAll` — the full `conflicts<>` vector — which is exactly the
SDK-testing signal we want.

Today that data goes nowhere useful for SDK testing:

- **tx-level (root) commit.** `try_commit_transaction()` calls
  `log_db_conflict_diagnostic()` (`syscall_handler.cc:575/587/711`), which only
  does `trace(...)` — i.e. writes to **stderr** (`base/log.h:262`). It is never
  returned to the FFI caller. The FFI `try_commit_transaction` returns only
  `ForeignTryCommitResult { result_code, ffi_error_code }`.
- **try_invoke sub-frame commit.** `invocation_frame.cc:313`
  (`try_acquire_frame_locks`, proposal mode) populates the same
  `db_proxy_.last_push_conflict()` when a `try_invoke` child's 2-phase push
  fails, but nothing surfaces it, and a later push overwrites it.

We want both surfaced through test_ffi so the SDK test harness can assert on the
conflict reasons: the tx-level conflict out of the `try_commit_transaction`
binding, and the try_invoke conflict inside the invocation trace.

## Goals

- Return the full tx-level `DbConflictResult` from the FFI
  `try_commit_transaction`.
- Make a failing `try_invoke` frame's `DbConflictResult` visible in that frame's
  entry in the serialized invocation trace.
- Emit the **whole** `DbConflictResult` union in both places (including the
  `kDbDiagnosticNone` arm) — never a null/absent "no conflict" sentinel — so the
  consumer always deserializes a well-formed, self-describing value. A wrong
  discriminant (e.g. `First` where `All` was expected) or a format drift is then
  immediately visible instead of masquerading as "no conflict".

## Non-goals

- No change to the conflict-detection logic, the set of `DbConflictCode`s, or
  the `DbDiagnosticLevel` selection.
- No new production surfacing (executor_context / scheduler paths are
  unchanged); this is test_ffi + the trace only.
- Pushing the `transaction_trace.x` protocol change through groundhog-go /
  protocol-go is handled separately by the maintainer, out of this spec.

## Design

### Part A — tx-level conflict out of `try_commit_transaction`

**Handler getter.** Add
`DbConflictResult const &SystemCallHandler::last_commit_conflict() const`
returning `db_proxy_.last_push_conflict()`. Reading it after commit is safe:
`abort_transaction()` never resets `last_push_conflict_` (only the next push
does, `database_proxy.h:156`), so the conflict from the failing push survives
until the handler is closed. This getter covers all three tx-level failure
sites — root lock-acquire (`syscall_handler.cc:575`), root 2pc push (`:587`),
and the `commit_body_discarded` RCQ/replay path (`:711`).

**FFI surface.** Add `struct FFIDataBuffer serialized_conflict;` to
`ForeignTryCommitResult` (`test_bindings.h`). In the FFI
`try_commit_transaction` (`test_bindings.cc:839`), after the handler call,
**always** XDR-serialize `last_commit_conflict()` into `serialized_conflict`,
regardless of level, reusing `serialize_to_ffi_data_buffer`. The `None`/`First`
arms are tiny; only the debug `All` vector allocates. The caller always frees
`serialized_conflict` with `free_ffi_data_buffer`.

The serialization is best-effort inside the existing `try`/`catch`: a
`bad_alloc` while serializing the `All` vector degrades to a null buffer rather
than turning a real commit result into an FFI error. The null buffer is thus
reserved for "serialization failed", not "no conflict" — no conflict is the
`kDbDiagnosticNone` arm of a well-formed buffer.

### Part B — try_invoke sub-frame conflict in the trace

**XDR.** In `protocol/transaction_trace.x`, add `%#include "db_conflict.hh"`
and a field to `InvocationFrameTrace`:

```
struct InvocationFrameTrace {
  Invocation invocation;
  SyscallTrace system_calls<>;
  FlaggedStorageDelta storage_writes<>;
  InvocationFrameOutput status;
  DbConflictResult commit_conflict;   // NEW
};
```

A plain value, not an optional pointer: `DbConflictResult` default-constructs to
the `kDbDiagnosticNone` arm, so every frame trace carries it — `None` on frames
that hit no try_invoke 2pc conflict, the real conflict on ones that did. This is
the protocol change the maintainer resyncs into groundhog-go.

**Population — the stale-read hazard.** `db_proxy_.last_push_conflict()` is reset
only at the *start of each push* (`database_proxy.h:156`). After a frame that did
**no** push during its finish (any non-try_invoke frame), it still holds a stale
value from an earlier frame's push. So the conflict must be handed out
explicitly from the one place that knows a push happened, not read globally.

Add an out-parameter to `InvocationFrameStack::finish_frame`:
`DbConflictResult *try_invoke_conflict_out = nullptr`. On the proposal-mode
try_invoke acquire branch (`invocation_frame.cc:318-321`), after computing
`acq_success`, set `*try_invoke_conflict_out = db_proxy_.last_push_conflict()`
(which is `None` on a successful acquire). This mirrors the existing
`conflict_out` out-param on `try_push_new_subtransaction`. All other paths leave
the out-param untouched, so the caller's local stays default `None`.

**Population — ordering.** Today `SystemCallHandler::finish_frame`
(`syscall_handler.cc:904`) restores the parent trace and moves the child trace
into the parent (`:931-957`) *before* calling
`invocation_frame_stack_.finish_frame` (`:960`). To write the conflict into the
finishing child's own trace, reorder:

1. `log_trace_finish(*invocation_frame_trace_)` — stays before the pop (reads the
   finishing frame's storage deltas/status).
2. Call `invocation_frame_stack_.finish_frame(..., &local_conflict)`.
3. While `invocation_frame_trace_` still points at the child, best-effort set
   `invocation_frame_trace_->commit_conflict = local_conflict`.
4. Then run the existing parent-restore + child-attach block.

The restore/attach block only touches trace objects (not the frame stack), so
moving it after the stack `finish_frame` is safe. Step 3 copies a
`DbConflictResult` (its `All` arm allocates), so it is wrapped in try/catch and
dropped on failure — `finish_frame` is `noexcept` and reachable from the test
FFI mock path, matching the surrounding best-effort trace handling (#2473 /
#2646 / #2741).

**Serialization — none.** `get_trace()` serializes the whole nested trace tree
via `serialize_to_ffi_data_buffer`, so a child frame's `commit_conflict` rides
out on the existing `serialized_trace` of `root_invoke` /
`finish_mock_invocation_frame`. No FFI serialization changes for Part B.

**Behavior note.** A try_invoke frame that fails its 2pc still records its
deterministic `status` as `Return` in the trace (its body succeeded; the
*commit* failed nondeterministically). `commit_conflict` is the new signal that
carries the 2pc failure reason.

### Part C — versioning and bindings

- Bump `TEST_FFI_VERSION_*` in `test_bindings.h`. The `ForeignTryCommitResult`
  layout change is ABI-breaking → minor bump (0.19 → 0.20).
- Regenerate the Rust bindings via `hogd/test_ffi/run_bindgen.sh` into
  `hogd/rust/test_ffi_bindings`.
- `transaction_trace.x` → groundhog-go resync handled separately by the
  maintainer.

## Files touched

| File | Change |
|------|--------|
| `protocol/transaction_trace.x` | `%#include "db_conflict.hh"`; add `commit_conflict` to `InvocationFrameTrace` |
| `hogd/hogexec/runtime/syscall_handler.h` / `.cc` | `last_commit_conflict()` getter; reorder + populate in `finish_frame` |
| `hogd/hogexec/runtime/invocation_frame.h` / `.cc` | `try_invoke_conflict_out` out-param on `InvocationFrameStack::finish_frame` |
| `hogd/test_ffi/test_bindings.h` | `serialized_conflict` field; version bump |
| `hogd/test_ffi/test_bindings.cc` | always serialize `last_commit_conflict()` in FFI `try_commit_transaction` |
| `hogd/rust/test_ffi_bindings` | regenerated |

## Testing

Tests are placed with the path/mode they exercise (repo convention).

- **`try_invoke_test.cc`** (C++ unit): a try_invoke that provokes a conflict
  class — start with `kDbConflictExclusiveLock`, the easiest to force — asserts
  the finishing frame's trace `commit_conflict` carries the expected code(s), and
  a non-conflicting frame's `commit_conflict.level() == kDbDiagnosticNone`. A
  root-level conflict asserts `last_commit_conflict()` directly.
- **`test_bindings_test.cc`** (test_ffi binding): drive a commit conflict through
  the FFI, deserialize `serialized_conflict`, assert
  `level() == kDbDiagnosticAll` (debug) and the expected `conflicts<>`. Drive a
  try_invoke conflict and assert it appears in the deserialized nested trace's
  `commit_conflict`.

## Risks / considerations

- **Protocol change.** Appending `commit_conflict` to `InvocationFrameTrace` is
  backward-compatible in XDR (appended field) but touches the shared protocol
  and requires a groundhog-go resync (owned by the maintainer, tracked outside
  this spec).
- **ABI break.** The `ForeignTryCommitResult` layout change is caught by the CI
  test_ffi version check; the minor bump + regenerated Rust bindings keep them in
  sync.
- **Debug vs release.** The `All` (full vector) arm is debug-only; release
  proposal yields `First` (single). Because both places emit the whole union, a
  consumer expecting `All` but receiving `First` sees it in the discriminant
  rather than silently getting one conflict.
