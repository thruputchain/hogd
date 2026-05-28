# RV32 C++ Ownership Review

This is the "would we want to own this forever?" pass. The answer is yes for the
VM core, with explicit caution around the protocol boundary.

## Verdict

The core is not scary.

`program.{h,cc}`, `machine.{h,cc}`, `host.h`, and `encoding.h` are small,
direct, and auditable. They validate once, execute decoded instructions, keep
code separate from guest memory, debit gas before mutation, and spell out the
RISC-V edge cases that C++ can otherwise get wrong.

The edge code is improving but still experimental:

- `syscall_bridge.{h,cc}` reuses `syscall_entry` correctly, but still uses a
  scratch-copy memory model per ECALL.
- `artifact.{h,cc}` and `dispatch.{h,cc}` are strict and well-tested, but their
  broad error classes should be split before heavy fuzzing.
- `runtime_context.{h,cc}` is useful test scaffolding, not final production
  contract selection.
- `fixtures/compiled_sha256_return/` is a regression fixture, not the SDK.

Recommendation: keep the in-tree VM path alive. Do not replace the core unless a
conformance oracle finds a real semantic defect.

## Core VM

What is strong:

- `Program::create` is the validation boundary.
- Execution runs over immutable `DecodedInstruction`, not raw bytes.
- `Machine::step` is easy to audit: fetch, gas, execute one instruction.
- `Machine::complete_ecall` is the single host-result commit point.
- State mutation ordering is visible in code.
- Code memory and data memory are separate.
- There is no JIT, relocation engine, signal machinery, or ambient runtime state.
- Tests pin gas-before-mutation, trap atomicity, `x0`, stack alignment,
  M-extension edges, bad jumps, ECALL completion, and representative programs.

Taste check: this is the part we should feel comfortable showing to a strong C++
reviewer.

Possible polish:

- Consider `std::expected` for `DecodeResult` / `ProgramResult` later.
- Add address/gas wrapper types only if they reduce real ambiguity.
- Keep growing conformance tests; readable code still needs independent oracles.

## Host Boundary

Closed issues:

- Host results now have typed payloads.
- Host trap status and payload are set atomically.
- Guest-caused bridge ABI faults classify as deterministic.
- Known host panic codes map to the same runtime outcomes as WASM.
- Host gas is debited before result commit.
- Fault/OOG/fatal paths do not write scratch memory back.

Remaining concern:

- The scratch-copy bridge is acceptable for the experiment, but it is not the
  final performance or allocation-determinism story.

## Artifact And Dispatch

Closed issues:

- Runtime does not parse ELF.
- Artifact v3 has SHA256 commitments for code, memory, and dispatch sections.
- Header offsets and little-endian helpers are named.
- Dispatch validation is shared by parser and runtime.
- Unknown method dispatch is a zero-gas deterministic miss.
- Gas digest includes RV32 instruction costs and live syscall costs.

Remaining concerns:

- `kInvalidOffsets` and `kDispatchTableInvalid` combine several failure causes.
  Split them before serious fuzz triage.
- The artifact format is exact-layout only. Keep that unless a new artifact
  version deliberately introduces sparse sections.
- Direct OpenSSL SHA256 in `artifact.cc` needs an activation-time equivalence
  check or should switch to the consensus wrapper if dependency shape allows it.

## Runtime Context

Current role:

- Test-only `ContractType::kMockTest` runtime.
- Proves RV32/WASM parity through the shared `SystemCallHandler`.
- Compares return buffers, traces, storage writes, events, stats, dispatch, and
  error outcomes.

Do not treat it as final production shape. M5/protocol integration should decide
how deployed artifacts are stored, verified, and selected.

## Fixture And Lowerer

Current role:

- Committed compiled Rust regression fixture.
- Reproducible metadata-backed build.
- Strict lowerer for the fixture: rejects unsupported allocated sections,
  relocations, executable non-text sections, overlapping data, data symbols in
  `.text`, and invalid dispatch symbols.

Do not treat it as the SDK:

- Method mapping is fixture-specific.
- Rust syscall glue is hand-written.
- Production lowerer needs a manifest or generated metadata section.
- Production `.text` data policy needs an enforceable rule for anonymous literal
  pools and future codegen patterns.

## Remaining M5 Readiness

Closed:

- Typed host result payloads.
- Atomic host trap payload handling.
- Deterministic bridge-fault classification.
- SHA256 artifact section commitments.
- Gas digest includes syscall costs.
- Differential RV32/WASM error outcome tests.

Still open before protocol exposure:

- Cross-platform deterministic runs.
- Production SDK lowerer design.
- RV32I/RV32M conformance corpus.
- Parser/dispatch fuzzing and sharper error codes.
- Activation-time SHA256 wrapper/equivalence decision.
- Activation-time gas version bump discipline.

## Bottom Line

We should like the core VM. It is small, explicit, and maintainable. The work
left is the normal cost of turning a promising execution experiment into
protocol infrastructure: conformance, fuzzing, SDK shape, artifact trust, and
activation rules.
