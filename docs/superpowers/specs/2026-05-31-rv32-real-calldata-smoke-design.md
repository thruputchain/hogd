# RV32 real-calldata smoke — design

**Status:** design (2026-05-31). First slice of Phase-3 prep from
[rv32-author-toolchain.md](../../rv32/rv32-author-toolchain.md): exercise the
rv32 SDK contract's **calldata-decode path** end-to-end with a real typed call,
instead of the current empty-calldata invocation that aborts.

## Problem

`LoweredSdkArtifactSmoke` invokes the lowered counter artifact via
`make_root_transaction(gas, method_id)`, which hardcodes `.calldata = Calldata{}`.
The counter's mutating methods take a `u64` (`increment(amount: u64)`, etc.), so
with empty calldata they abort before doing anything — the smoke proves
parse+dispatch+entry, but never exercises the guest's **argument decode** or a
clean value-returning `Return`. We want a smoke that passes real calldata to a
real method and verifies the round-trip.

This is **not** blocked on the Phase-3 runtime-dispatch decision (Geoff): the v3
runtime already maps `method_id → PC` via the dispatch table and jumps to the
method's entry. The only missing input is the encoded `u64`. The calldata wire
format is the engine-agnostic host↔guest invoke ABI — the same one wasm
contracts use — so Geoff is the review point for the ABI, not a blocker.

## Background — the pattern to mirror

`fibonacci_test.cc` already drives a contract method with a scalar arg, from a
**native driver contract**, using a generated typed client:

```cpp
using fib_client_t = ContractTraitClient<fibonacci::Fibonacci>;
fib_client_t client(kFibContractAddr);
auto decoded = client.invoke_opt<"fibonacci">(ctx, input);   // ctx = NativeSyscallContext
```

`ContractTraitClient<Trait>` (in `contract_trait_client.h`) does the work via the
trait's compile-time helpers:
- `make_calldata<Name>(args...)` — encodes the calldata (the codec we reuse),
- `entrypoint<Name>()` — the name→method_id mapping (no hand-computed hash),
- `decode_return<Name>(...)` — decodes the typed return.

The trait type (`fibonacci::Fibonacci`) is a **generated header**:
`build/built_contracts/fibonacci.h`, produced by the contract codegen (the
`all_wasm_headers`-style step `rv32_runtime_context_test` already depends on).

As of this spec, that part is **not** true for `rv32_smoke_counter`: the RV32
smoke build produces the lowered artifact (`.rv32a`) and lowerer metadata
(`.rv32.json`), but it does not produce a `rv32_smoke_counter_spec.json` or
generated C++ trait header. Creating that trait source is therefore the first
implementation task, not just a confirmation step.

## Goal / non-goals

**Goal.** A test that invokes the lowered rv32 counter's `increment(u64)` with a
correctly-encoded calldata payload and asserts the round-trip
(`increment(N)` runs to a clean `Return`, then `get()` returns `N`) — exercising
the full host→guest calldata-decode path with a guaranteed-correct encoding.

**Non-goals.**
- No guest-side dispatcher / method_id-passing work (that's the v4 / Geoff
  track). We use the existing v3 `method_id → PC` dispatch unchanged.
- No new calldata codec — reuse the trait's `make_calldata` (Approach A).
- No `make_root_transaction` calldata plumbing — we drive via a native contract
  (mirroring fibonacci), so the typed client owns encoding + entrypoint.

## Design

### Architecture

A **native driver contract** (built with `NativeContractBuilder`, as in
`fibonacci_test`) holds a `ContractTraitClient<Counter>` pointed at the installed
rv32 counter contract, and in a single invocation does:

1. `client.invoke_opt<"increment">(ctx, N)` — cross-contract `kInvoke` with
   `make_calldata<"increment">(N)`,
2. `client.invoke_opt<"get">(ctx)` — reads the value back,
3. returns success iff the decoded `get()` value equals `N`.

The test `root_invoke`s the driver. Both child calls run within the one
transaction, so the `increment` storage write is committed and visible to the
subsequent `get` (same pattern fibonacci relies on).

### Components

1. **Generated Counter trait** (`counter::Counter` analog of
   `fibonacci::Fibonacci`). The codegen must emit a C++ trait header for the
   `rv32_smoke_counter` contract from its contract spec, the same way it does for
   wasm contracts. This is the first implementation task:
   - produce a `rv32_smoke_counter_spec.json` for the RV32 smoke contract,
     either by adding a spec-only/WASM-compatible build of the same fixture or
     by teaching the RV32 `groundhog build --target rv32` path to emit the
     contract spec sidecar;
   - run `built_contracts/generate_header.py` on that spec to produce
     `rv32_smoke_counter.h`;
   - because the contract declares `throws = Error`, generate or include any
     required XDR/type header the same way existing throwing contracts do; and
   - wire the generated header as a build dependency of
     `rv32_runtime_context_test`.

   Current shape to account for: `rv32_smoke_counter` is an `rlib` plus an RV32
   `[[bin]]` wrapper that links `groundhog-rv32-runtime`; it is not currently
   just another `add_wasm_builds(SPEC_CONTRACTS ...)` entry.

2. **New test** `LoweredSdkArtifactCalldataRoundTrip` in
   `rv32/rv32_runtime_context_test.cc`. Reuses the existing artifact-load +
   `RuntimeContractImage` setup from `LoweredSdkArtifactSmoke` (factor the shared
   load/parse into a helper so the two tests don't duplicate it), installs the
   rv32 counter, builds the native driver above, and asserts the round-trip.
   The runtime context must be a `MultiplexedContext` with `ContractType::kNative`
   routed to the native driver context and `ContractType::kMockTest` routed to
   `rv32::RuntimeContext`, because the current RV32 runtime is still modeled as a
   mock-test contract type.

### Data flow

```
root_invoke(driver tx)
  └─ native driver frame
       ├─ invoke_opt<"increment">(ctx, N)
       │    calldata = Counter::make_calldata<"increment">(N)
       │    kInvoke → runtime maps method_id(increment) → PC → rv32 guest
       │    guest macro decodes the u64, strict_inc(value, N), Return
       └─ invoke_opt<"get">(ctx)
            kInvoke → method_id(get) → PC → guest reads value, returns u64
       assert decoded get() == N
```

### Error handling / signals

- The round-trip is the assertion: a wrong calldata encoding or a broken decode
  path yields an `Abort`/`Fault` child frame or a mismatched value → the test
  fails loudly (unlike the informational `LoweredSdkArtifactSmoke`, this is a
  hard `ASSERT`/`EXPECT_EQ`).
- Keep `LoweredSdkArtifactSmoke` as-is (informational parse+dispatch coverage).

### Testing

- `LoweredSdkArtifactCalldataRoundTrip`: `increment(N)` then `get() == N` for a
  representative `N` (e.g. 7). Optionally a second `N` to guard against a
  constant-folded decode.
- Runs when the generated artifact, lowerer metadata, and generated Counter
  header are present. The existing CMake target currently depends on building the
  RV32 smoke artifact before test discovery, so a missing RV32 toolchain may fail
  at build time rather than reaching a `GTEST_SKIP`; if skip-on-missing remains a
  requirement, the CMake dependency needs to become optional or environment-gated.

## Geoff touchpoint

The calldata ABI is reused, not invented — `make_calldata` is the same encoder
wasm contracts use, and the rv32 guest decodes it with the same SDK macro. The
review point for Geoff is confirming the rv32 guest's calldata-decode genuinely
matches that ABI (it should — one macro, one codec); if the round-trip fails on
encoding, that's the ABI seam to examine with him.

## Risks / open items

- **Counter trait generation (primary task):** the current RV32 smoke producer
  does not emit a contract spec or C++ trait header. This slice must either add a
  compatible spec-generation path for `rv32_smoke_counter` or switch hogd's RV32
  fixture build to a `groundhog build --target rv32` path that emits the spec
  sidecar.
- **Throw type/header wiring:** `rv32_smoke_counter` declares `throws = Error`;
  if the generated C++ trait models that exception type, the build must also
  generate/include the corresponding type header.
- **Native/RV32 multiplexing:** the driver and callee require a mixed runtime
  context (`kNative` + `kMockTest`). Reuse the `fibonacci_test` driver pattern
  and the RV32 context setup, but make the contract-type routing explicit.
- **Cross-call state visibility:** `increment` then `get` must share committed
  storage within the driver's transaction. This is the same assumption
  `fibonacci_test`/cross-contract tests already rely on; verify with the
  round-trip assertion.
- **`NNInt` from-unset semantics:** `increment(N)` on a never-initialized
  `NNInt` should yield `N` via `strict_inc`; if it requires `initialize` first,
  the driver calls `initialize<"initialize">(N)` instead and `get()` asserts `N`.

## Acceptance criteria

1. A native driver invokes the lowered rv32 counter's `increment(u64)` with
   calldata encoded by the reused `make_calldata` codec — no hand-rolled bytes,
   no hand-computed method_id.
2. The round-trip asserts `get()` returns the value passed to `increment`.
3. A wrong encoding or broken guest decode fails the test (hard assertion).
4. `make_root_transaction` and the v3 dispatch model are unchanged; no
   guest-side method_id work.
5. The build produces the Counter C++ trait from the contract spec and wires it
   into `rv32_runtime_context_test`.
6. The test behavior around missing RV32 toolchain/artifact inputs is explicit:
   either the CMake build requires them, or the target is made optional enough
   for the GTest body to skip cleanly.
