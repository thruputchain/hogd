# RV32 C++ Ownership Review

This note is the "would we want to own this forever?" pass after Milestone 4.
The bar is deliberately higher than "the experiment works." If RV32 moves
toward protocol exposure, this becomes consensus-adjacent infrastructure that
Groundhog owns directly instead of delegating to a vendored VM.

The most important question is whether the VM core is small, explicit, and
unscary enough that we could show it to our strongest C++ reviewer and say:
"this is well done; the risks are visible; we can maintain it."

## Verdict

The VM core passes that test.

`program.{h,cc}`, `machine.{h,cc}`, `host.h`, `encoding.h`, and the core VM
tests are compact and mechanically legible. The design is not clever in the
dangerous sense. It validates once into an immutable decoded program, executes a
small switch over explicit operations, debits gas before mutation, keeps code
memory separate from guest data memory, and centralizes the few architectural
rules that are easy to get wrong.

The code is not yet "forever code" at the runtime boundary. The remaining
ownership concerns are mostly at the edges: overloaded host-result payloads,
bridge fault classification, non-cryptographic artifact digests, gas-schedule
versioning that does not yet include syscall costs, and fixture/lowerer code
that is intentionally pre-SDK. Those are M5 hardening items, not reasons to
discard the core VM.

Recommendation: keep the in-tree VM path alive. Before M5, spend cleanup effort
on typed edge contracts and protocol-grade artifact/gas commitments. Do not
rewrite the VM core unless a differential ISA test finds a semantic defect.

## Review Lens

This pass uses four criteria:

- Local reasoning: can a maintainer understand a trap, gas, or state mutation
  path without reconstructing the whole runtime?
- Type pressure: do important states have names and structure, or are they raw
  integers selected by convention?
- Consensus discipline: are validation, gas, mutation, rollback, and halt/trap
  ordering visible in code?
- Taste: is the code direct, constrained, and boring in the right places?

## Core VM: Keep

Files: `hogd/hogexec/runtime/rv32/program.{h,cc}`,
`hogd/hogexec/runtime/rv32/machine.{h,cc}`,
`hogd/hogexec/runtime/rv32/host.h`,
`hogd/hogexec/runtime/rv32/encoding.h`.

What works well:

- `Program::create` is the consensus validation boundary. Raw code bytes are
  decoded once, unsupported instructions are rejected up front, and `Machine`
  runs over `DecodedInstruction` instead of reinterpreting byte streams.
- The decoded instruction shape is plain data. `InstructionKind`, `Op`,
  decoded registers, immediates, and per-op gas are visible without an
  inheritance hierarchy or an opaque interpreter framework.
- The gas schedule is explicit data with a compile-time shape check and a
  digest. This is the right direction for code we own.
- `Machine::step` has the right ownership shape: validate fetch, debit gas,
  execute one instruction, expose `ECALL` as a step result, and keep host
  completion in a separate `complete_ecall` path.
- Mutation ordering is easy to audit. Control-flow targets are checked before
  link-register writes; loads/stores check after gas debit but before register
  or memory mutation; `x0` behavior is centralized in `write_reg`.
- RISC-V edge cases are called out where C++ would otherwise be dangerous:
  signed division overflow, division by zero, arithmetic shift, signed bit
  interpretation, and shift masking.
- Code and data are intentionally separate. Guest writes cannot mutate the
  decoded program image.
- The public surface is small: `Program`, `Machine`, `MachineView`, `Host`, and
  simple result enums.

What keeps it from being scary:

- There is no JIT, generated interpreter, dynamic relocation, or platform
  signal machinery.
- The core is deterministic by construction: no host allocation on normal
  instruction execution, no ambient runtime state, no threading, no callback
  graph inside instruction execution.
- The tests directly pin the invariants a maintainer cares about: register
  zero, aligned stack, branch/jump traps, gas-before-mutation, load/store trap
  atomicity, M-extension edge cases, `ECALL` step completion, randomized store
  bounds, and same-input determinism.

Cleanup before showing the core as final:

- Consider a small `Pc`/`GuestAddress`/`GasAmount` type layer only if it reduces
  actual ambiguity. The current raw `uint32_t` use is acceptable because the
  code is small and helper names are clear. Do not add wrappers for style alone.
- Consider replacing `DecodeResult`/`ProgramResult` bool-plus-error structs with
  `std::expected` for consistency with `Program::create`, but this is polish,
  not a correctness issue.
- Keep importing or recreating external RV32I/RV32M conformance cases. The code
  is readable enough to own, but ISA correctness still wants independent
  oracles over time.

Call: keep. This is code we can plausibly own.

## Host And Syscall Boundary: Keep With Cleanup

Files: `host.h`, `machine.cc`, `syscall_bridge.{h,cc}`,
`runtime_context.cc`.

This is the least elegant part of the current stack, even though the behavior is
now correct and well tested.

What works well:

- `Host` is a narrow interface. The VM does not know about `SystemCallHandler`,
  WASM internals, contract frames, or storage/event machinery.
- `Machine::complete_ecall` is the right commit point. Host gas is debited after
  ECALL instruction gas and before committing `a0`, halt state, host traps, or
  host memory effects.
- Memory rollback is explicit for host out-of-gas and over-budget host gas.
- The bridge deliberately reuses the production-shaped `syscall_entry` path, so
  parser, output, trace, and gas behavior are not reimplemented in the VM core.
- Fault writeback gating is now conservative enough for M4: parser/output
  faults, OOG, bridge traps, and fatal host panics do not commit scratch memory.
- `HostResult` now carries status-specific typed payloads instead of a shared
  return/halt/trap integer, and host traps carry an explicit deterministic vs
  fatal kind.
- `Machine` sets `Trap::kHostTrap` and its payload together, avoiding a
  convention-only `trap_` / code split.
- Guest-caused bridge ABI faults now classify as deterministic runtime
  failures: unsupported bridge version, misaligned frame pointer, and
  unreadable syscall frame are pinned by runtime tests.

Ownership concerns:

- `SyscallBridgeHost` still makes a full scratch memory copy per ECALL. This is
  fine for M4 and useful for atomicity, but it is not the final performance or
  determinism story.

M5 cleanup:

- Revisit scratch-copy writeback after typed fault classification is in place.

Call: keep with cleanup before M5.

## Artifact And Dispatch: Keep With Cleanup

Files: `artifact.{h,cc}`, `dispatch.{h,cc}`, `encoding.h`.

The artifact parser is much stronger than the early experiment required. It is
strict, little-endian, versioned, header-size pinned, profile pinned, ABI pinned,
gas-digest checked, bounds checked with widened arithmetic, and canonical about
layout.

What works well:

- The runtime does not parse ELF. It receives a small Groundhog artifact with
  explicit code, memory, dispatch, and version metadata.
- Header offsets are named in `artifact.h` and little-endian helpers are shared.
- Dispatch entry encoding is documented as 16 bytes with explicit reserved
  zero bytes.
- Dispatch validation is shared in `dispatch_table_is_valid`, so parser,
  runtime verification, and runtime invocation do not drift on sorted IDs or PC
  validity.
- Unknown method dispatch is resolved before VM construction and consumes zero
  gas, matching the WASM export-not-found shape.

Ownership concerns:

- Artifact section digests are now SHA256 commitments in the v3 header. The
  remaining non-cryptographic digest is the versioned gas-schedule fingerprint,
  which is an intentional compatibility/version pin rather than a content
  authenticity check.
- `ArtifactError::kInvalidOffsets` and `kDispatchTableInvalid` each combine
  several conditions. That is acceptable for M4 tests, but fuzz triage and
  operational debugging will want sharper error classes.
- The parser is intentionally exact-layout only. That is good for M4
  determinism, but it means any future sparse section format should be a new
  version, not an in-place relaxation.

M5 cleanup:

- Closed: replace FNV artifact digests with SHA256 section commitments.
- Closed: fold syscall gas costs into the gas-schedule digest so total RV32
  cost is versioned, not only instruction cost.
- Split the broad artifact/dispatch errors enough for fuzzing and review.
- Keep the canonical layout rule unless a new artifact version explicitly
  changes it.

Call: keep with cleanup before M5.

## Runtime Context: Experiment Only Until M5 Shape Is Chosen

File: `runtime_context.cc`.

The runtime context is intentionally test-only and still restricted to
`ContractType::kMockTest`. That isolation is good. It also means we should not
over-invest in polishing it as if it were the final production contract
selection layer.

What works well:

- No production `ContractType::kRv32` path exists.
- Runtime dispatch is auditable through the artifact table before VM execution.
- Host-visible success parity is now meaningful: SHA256, Return, storage writes,
  events, trace bodies, stats, and multi-method dispatch are compared to WASM.
- Host panic outcomes mirror WASM for the known `HostFnErrorCode` cases.

Ownership concerns:

- `RuntimeContractImage` is convenient test plumbing, not a final deployed
  artifact store model.
- Error-path parity is partly construction-by-switch plus mock entries. Before
  protocol exposure, WASM-vs-RV32 error contracts should prove validation
  failure, abandon, deterministic guest failure, and host trap classes
  differentially.

Call: keep as experimental scaffolding. Redesign deliberately for M5/protocol
integration.

## Compiled Fixture And Lowerer: Experiment Only

Files under
`hogd/hogexec/runtime/rv32/fixtures/compiled_sha256_return/`.

This code proved the toolchain path and should not be mistaken for the final SDK
lowerer.

What works well:

- The fixture is reproducible, metadata-backed, and committed.
- Build inputs are hashed, including Cargo config and lockfile.
- Entry functions are force-kept by linker script and referenced by a `#[used]`
  table.
- The lowerer rejects unsupported allocated sections, relocations, executable
  non-text sections, overlapping data sections, object/data symbols in `.text`,
  and bad dispatch symbols.
- Method IDs are statically tied to generated WASM contract metadata in tests.

Ownership concerns:

- Method ID mapping is fixture-specific. The real SDK lowerer should not
  hardcode symbol names and selector constants in Python.
- `.text` purity still depends partly on symbol visibility. Anonymous literal
  pools or future codegen patterns need a production policy.
- The Rust syscall glue is deliberately disposable and pre-`sdk_rs`.

M5 or SDK cleanup:

- Move method mapping to a manifest or SDK-generated metadata section.
- Decide the production `.text` data policy with an enforceable lowering rule.
- Replace hand-written syscall frames with typed SDK wrappers.

Call: experiment only. Keep it as a regression fixture, not as product shape.

## Would We Vendor Instead?

The case for owning this VM is now credible because the core is small and
specific. Vendoring a general RISC-V VM would likely add surface area we do not
want: privileged features, dynamic memory models, ELF assumptions, JIT/AOT
options, host abstractions, or broad compatibility code. The in-tree VM is
narrower than a general VM and easier to reason about as consensus code.

The strongest argument against owning it is long-tail ISA correctness and
cross-platform determinism. That argues for stronger conformance and
differential testing, not necessarily for vendoring. If we vendor, we still own
the artifact format, gas schedule, host boundary, syscall ABI, and consensus
error mapping. Those are the parts most likely to matter for protocol behavior,
and they are already ours either way.

Recommendation: continue owning the VM core, but treat independent conformance
and differential testing as part of the cost of ownership.

## M5 Readiness Checklist

Completed in M5 host-boundary hardening:

- Typed host result payloads and atomic host trap-code handling.
- Deterministic classification for guest-caused bridge ABI faults.
- Cryptographic artifact content commitments.
- Gas schedule digest covers both RV32 instruction costs and syscall costs.

Before RV32 becomes protocol-visible:

- Artifact verification uses the consensus SHA256 wrapper, or carries an
  explicit equivalence check for the direct OpenSSL verifier used by rv32_vm.
- Any digest-affecting gas schedule change bumps `kGasTableVersion`; during the
  experiment the digest itself is already pinned and validated.
- Differential RV32/WASM error contracts for deterministic failure,
  validation failure, abandon, OOG, and host trap classes.
- Cross-platform deterministic test runs on supported validator platforms.
- Production SDK lowerer design: method manifest, typed syscall wrappers,
  explicit `.text` data policy, and reproducible artifact metadata.
- Imported or recreated RV32I/RV32M conformance corpus.

## Bottom Line

The part we most need to like is the VM core, and we should like it. It is
direct, explicit, and small enough to audit. The code that still needs work is
mostly at the boundary where experimental VM execution becomes protocol
semantics. That is a good place to be: the hard-to-replace core is not scary,
and the scary-to-get-wrong edges are already identified as M5 gates.
