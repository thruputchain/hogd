# Syscall error-code precedence audit

Date: 2026-08-16. Goal: find every place where a syscall can return more than
one error code and the *order of checks* decides which — and which of those
orderings have no pin. Sources: full enumeration of every inner syscall's
check order + the wrap_outer stages, diffed against every existing error-code
assertion (syscall_misc_tests + its Rust guest, auth token/config tests,
parse/alias/output-fault tests, rcq tests, syscall_error_handling_tests, mock
FFI tests, trace_shape_tests).

## The wrapper precedence spine (applies to every syscall)

wrap_after_parse order: trace-start → **P1 argument parse fault**
(`kSyscallErrorCodeFault`, gas `kGenericBadSyscallCost`, bumps
`syscall_fault_count`) → **P2 alias-guard fault** (same code/gas, does NOT
bump the fault counter) → **P3 read-only-frame rejection of mutable syscalls**
(`kSyscallErrorCodeInvalidSyscallInput`, gas 0) → inner call (throw →
`Unrecoverable`, gas 0) → **P10 gas-overrides-any-handleable-result**
(`OutOfGas`) → **P11 no-return-syscall Exit override** (globally dead today:
only Return/Exit have kCanReturn=false and both inners already return
HostFnError) → disarm outputs on error.

Mock-level pins exist for the gas-override lattice
(syscall_error_handling_tests: Abandon/Unrecoverable/ValidationFailure beat
OutOfGas; OutOfGas beats Return/Exit/allowed-error; read-only precedes inner
and gas; throws → Unrecoverable gas 0).

## Unpinned precedence pairs with DISTINCT observable codes (top priority)

1. **RCQ Consume + PeekLen: read-cap vs empty queue.** Cap check precedes
   empty check (`syscall_handler.cc:1880` before `:1891`) →
   `RetrocausalQueueReadLimitReached` wins. Both are contract-handleable stop
   conditions with different semantics. Unpinned.
2. **RCQ Consume/PeekLen: wrong-type key vs cap reached.** Type dispatch
   falls through to `InvalidSyscallInput` (`:2034`/`:1935`) before
   `load_next_queue_entry` runs → `InvalidSyscallInput` wins. Unpinned.
3. **GetReturnedBuffer: no buffer available vs misaligned
   restored_auth_token_ids span.** `NoReturnBufferAvailable` wins
   (`syscall_invoke.cc:315` before `:320`) — the opposite convention from
   GetCalldata, where alignment is checked first. Unpinned.
4. **Invoke: reentrant target vs nonexistent target.** `Reentrance` wins
   (`syscall_handler.cc:790` before `:854`). Unpinned. (Flags-beat-reentrance
   IS pinned by TestInvokeInvalidFlags.)
5. **Invoke: unowned/duplicate forwarded tokens vs reentrant target.**
   Token checks (`:786`, inner `:193`) precede reentrance (`:790`) →
   `InvalidSyscallInput` wins over `Reentrance`. Unpinned.
6. **DeployContract: unknown contract hash vs already-deployed address.**
   `InvalidSyscallInput` wins over `ContractAlreadyDeployed`
   (`syscall_handler.cc:1548` before `:1569`). Unpinned.
7. **P1 vs P3: OOB argument on a mutable syscall in a read-only frame.**
   `Fault` at gas `kGenericBadSyscallCost` beats `InvalidSyscallInput` at gas
   0 — distinct code AND distinct gas. Unpinned.
8. **P2 vs stats: an aliasing-but-in-bounds call is a Fault that does NOT
   increment `syscall_fault_count`; a parse fault does.** The counter is the
   only observable distinguishing P1 from P2. `syscall_fault_count` is never
   asserted nonzero anywhere. Unpinned.

## Unpinned same-code orderings that are state- or boundary-observable

9. **SHA256Update: oversized data on reset=true of an existing buffer fires
   BEFORE the clear** (`hash_buffers.cc:40` before `:44`) — the old buffer
   survives a rejected reset and can still be finalized to its prior hash.
   Reversing the order would destroy it. Pinnable via update(oversize,
   reset=true) → error, then finalize → old digest. Unpinned.
10. **SHA256Update boundary asymmetry:** `data.size() == kSHA256BufferMaxSize`
    succeeds with reset=true but fails when appended to a nonempty buffer
    (cap compares `existing+data` on append, `data` alone on reset).
    Accept-side boundary unpinned (only the reject side is).
11. **SHA256Update: table-full vs oversized data on a fresh index** —
    table-full wins (`:48` before `:52`); same code, no state effect. Low.
12. **ConsumeAuthToken sub-cases are entirely unpinned at the syscall level:**
    unknown token id (`:2256`), expired token (`:2261`), not-owned
    (`invocation_frame.cc:570`), wrong consumer_address (`:576`) — all
    `InvalidSyscallInput`, checked in that order (unknown→expired→unowned→
    wrong-consumer; note ownership is checked AFTER expiry). Only the
    read-only-child rejection is pinned. Sub-case pins would catch a future
    code split or reorder.
13. **rcq_delete_test's shared helpers deliberately accept EITHER
    `kNoRetrocausalQueueEntry` OR `kStorageKeyNotExist`**
    (`rcq_delete_test.cc:113-115` etc.) — the deleted-vs-empty distinction is
    explicitly not pinned today. Decide the canonical code and tighten.

## Unobservable-but-load-bearing orderings (document, can't pin by output)

- VerifySignature: size check before `subspan<0,N>` is memory-safety
  load-bearing; scheme dispatch before size. All collapse to
  `InvalidSyscallInput`.
- MintAuthToken: expiry check deliberately precedes XDR parse (DoS shaping,
  same rationale as sort-based duplicate detection). Expiry/parse/depth/
  registry order all collapse to `InvalidSyscallInput`.
- Return: all five violations collapse to Exit; precedence provably
  unobservable (gas 0 on every path). Already fully pinned per-condition.
- ChangePublicKeyRegistration: arg-shape lambda runs before the handler's
  key_index bound — an OOB index is never the reported cause when args are
  also malformed. Same code.

## Meta/doc inconsistencies (cheap fixes)

- **WriteAddressRequiredWeight is the only syscall in syscalls.yaml with no
  `errors:` key at all**, yet P3 can produce `InvalidSyscallInput` (pinned at
  mock level). Add the `errors:` entry (protocol doc change).
- Dead checks: LogEvent `num_topics > kMaxTopics` and
  `data.size() > kMaxEventLength` are unreachable via the syscall (subsumed /
  bounded); Invoke's `validate_new_invocation` flag re-check is dead on the
  real path (test-FFI only, documented); wrap_outer P11 is globally dead
  defence-in-depth.
- syscall_misc_tests numbering skips #24 and #32 and reuses #36 — check
  whether two planned cases were never written.
- NonnegativeIntGet: with `force_load_from_snapshot=false` EVERY error path
  returns success `(nullopt, 0)` — deleted, absent, wrong-type. The
  force_load=false wrong-type-success case is unpinned (misc tests pin only
  the force_load=true errors).

## Existing pin coverage (summary)

Single-code coverage is broad: syscall_misc_tests #1-#36 + auth token/config
suites pin at least one test for nearly every (syscall, code) pair; parse
hardening pins optional-pointer sentinel vs OOB fault; output-fault tests pin
output disarming under Fault regardless of argument position; alias tests pin
input/input, output/output, and input/output aliasing→Fault. GetBlockHeight
has no error pin (only failure is a SystemError; acceptable). Reentrance on
the real path is pinned by SelfInvokeIsRejectedAsReentrance.
