# Experimental RV32 VM Design

## Context

Groundhog currently executes user-deployed contracts through the WASM runtime
path. The production runtime is intentionally constrained for determinism and
metering: one-page linear memory, no floats, explicit fuel, structural import
guards, and a narrow `syscall_entry` / `SystemCallHandler` host boundary
currently exposed to WASM as `marmot.syscall`. That shape works, but it also
means the chain inherits complexity and performance shifts from the WASM engine.

This design introduces an experimental native C++ RV32 VM as a hedge. The first
milestone is deliberately not protocol-visible: no new contract type, no SDK
target, and no deployment path. The goal is to learn whether a small
deterministic interpreter can stay simple enough to carry in-tree while matching
Groundhog's execution model.

WASM remains the production path.

A separate, non-binding note on the zero-knowledge optionality this VM choice
opens — and the architectural properties worth preserving through M5 — lives in
the product-docs WIP area at `groundhog-product/wip/rv32-zk-optionality.md`.

## Goals

- Define a Groundhog-specific RISC-V execution profile:
  `groundhog-rv32im-ilp32-v1`.
- Build a native C++ VM library that can validate and execute small RV32IM
  programs under deterministic gas.
- Keep the implementation isolated from production contract execution until the
  VM passes conformance, fuzzing, and benchmark gates.
- Preserve the host-side Groundhog model: NNInts, storage conflict rules, native
  token conservation, replay/proposal behavior, and syscall semantics remain
  outside the VM.
- Treat RV32 instruction gas as a new deterministic schedule, not a port of the
  current WASM fuel model.

## Non-Goals

- No `ContractType::kRv32` in the first milestone.
- No SDK build target or Rust `riscv32im-unknown-none-elf` support yet.
- No production host syscall bridge yet.
- No JIT or AOT compiler.
- No floating point, atomics, vectors, threads, compressed instructions, or
  RISC-V privileged architecture.
- No native C++ user contracts.

## Alternatives Considered

Building the VM in-tree is a deliberate choice, not the only option. The RISC-V
ecosystem already includes mature engines, several built for deterministic,
metered smart-contract execution. Each was evaluated against this effort's actual
constraint — owning a small, stable core rather than inheriting an external
engine's complexity and performance drift — and rejected as the production core,
with one adopted only as a test oracle.

- **Rust consensus VMs — rejected as the core.** [PolkaVM][polkavm] (Parity) and
  [CKB-VM][ckbvm] (Nervos) are the closest analogs to "wasmi, but for RISC-V":
  both are designed around deterministic metering of untrusted code, and CKB-VM
  has run in production for years. Both are Rust. Adopting either re-creates the
  exact dependency this effort exists to hedge against — a large external engine
  whose behavior shifts outside our control — only with a different upstream than
  wasmi. PolkaVM's recompiler/JIT work is valuable prior art, but this profile
  forgoes JIT/AOT (see Non-Goals) because cross-platform JIT determinism is its
  own hard consensus problem. Both remain valuable as prior art: their
  deterministic metering models, entry ABIs, syscall boundaries, and memory models
  should inform ours rather than be re-derived from scratch.
- **Embeddable C++ interpreters — rejected as the core.** [libriscv][libriscv] is
  a small, embeddable C++ RV32/64/128 emulator, which fits the in-tree goal, but
  it targets low-latency scripting (for example, game engines), not consensus: it
  provides no gas metering and does not guarantee the determinism-by-construction
  this profile requires. Using it would mean auditing and re-hardening a foreign
  codebase for properties it was not designed to provide.
- **Reference models — adopted as a test oracle, not the runtime.**
  [Spike][spike] (`riscv-isa-sim`) and [sail-riscv][sail] are the canonical ISA
  references. Neither is a metered consensus VM, but one of them is the cheapest,
  highest-leverage piece of "buy" available: a differential-test oracle for decode
  and execution conformance. This is independent of the runtime build-vs-buy
  decision and should be adopted regardless (see Review Gates).

The case for building the core is stronger here than it was for WASM. RV32IM is
small enough that the decode and execution surface is closer to dozens of
instruction families than to a full WASM engine, so a correct, audited
interpreter is realistically a few thousand lines. Combined with the Non-Goals
(no JIT, no AOT, narrow dependencies) and the milestone goal of learning whether
a small deterministic interpreter can stay simple enough to carry in-tree, the
decision is to build the interpreter — but not to build the reference model or
design the gas schedule from a blank page.

[polkavm]: https://github.com/paritytech/polkavm
[ckbvm]: https://github.com/nervosnetwork/ckb-vm
[libriscv]: https://github.com/libriscv/libriscv
[spike]: https://github.com/riscv-software-src/riscv-isa-sim
[sail]: https://github.com/riscv/sail-riscv

## VM Profile

The v1 profile is `groundhog-rv32im-ilp32-v1`:

- ISA: RV32I plus M extension.
- ABI model: ILP32-style integer register conventions.
- Endianness: little-endian.
- Memory: bounded byte-addressed linear memory owned by the VM instance.
- Memory limit: Milestone 1 caps linear memory at 64 KiB, matching the current
  one-page WASM limit unless a later profile version changes it.
- Execution: single-threaded, deterministic, no interrupts.
- Host boundary: `ecall` only.
- Integer behavior: RV32 wrapping arithmetic as specified by the unprivileged
  ISA.
- Floating point: unsupported; all F/D/Q instructions trap as invalid.
- Atomics: unsupported; A-extension instructions trap as invalid.
- Compressed instructions: unsupported in v1; instruction fetch is 4-byte
  aligned.

The supported register set is the standard 32 integer registers. Register `x0`
is hardwired to zero after every instruction. The VM exposes ABI names only for
debugging and tests; the implementation stores registers by numeric index.

## Instruction Coverage

Milestone 1 should cover the full RV32I base integer set plus M extension:

- RV32I: `LUI`, `AUIPC`, `JAL`, `JALR`, conditional branches, integer loads,
  integer stores, OP-IMM, OP, `FENCE`, and `SYSTEM`.
- M extension: `MUL`, `MULH`, `MULHSU`, `MULHU`, `DIV`, `DIVU`, `REM`, `REMU`.
- `ECALL` is the only supported SYSTEM operation.

Unsupported or malformed encodings produce `Trap::InvalidInstruction`.
`EBREAK`, CSR operations, privileged instructions, and reserved encodings are
invalid in this profile. `EBREAK` is a valid RISC-V encoding, but
`groundhog-rv32im-ilp32-v1` rejects it deliberately.

`FENCE` has no host-visible memory effect in the single-threaded VM, but it is a
valid instruction and consumes gas.

## Program, Memory, And Entry

The VM uses a separate immutable code segment. Guest linear memory is data,
stack, and heap only; it is not executable, and self-modifying code is not part
of this profile. `Program::validate()` owns validation of the immutable
instruction image, and `Machine` fetches instructions only from that image.

The VM has a fixed linear memory size selected at instance construction, bounded
by the profile memory limit. Unless a test provides explicit initial memory,
linear memory starts zeroed.

Initial machine state:

- `pc = 0`.
- `x0 = 0`.
- `x2` / `sp` is the linear memory size rounded down to a 16-byte boundary.
- All other integer registers start at 0 unless explicitly initialized by a
  test helper.
- Gas starts at the caller-supplied gas limit.

Normal termination is not implicit. Running off the end of the program image is
`Trap::BadPc`. A program halts only when an `ecall` host result returns
`HostResult::Halt`.

## Traps

Trap rules are part of the VM spec:

- Instruction fetch requires `pc % 4 == 0`.
- Instruction fetch must address a complete 4-byte instruction in the validated
  program image.
- Bad `pc` fetch traps happen before gas is charged. After a complete
  instruction is fetched, the VM charges that instruction's gas before
  profile/semantic traps. If the remaining gas is insufficient, `OutOfGas`
  wins over those later traps.
- Loads and stores must be naturally aligned for their width.
- Loads and stores must be fully in bounds.
- `JALR` clears bit 0 of the computed target, as in RISC-V.
- Division by zero and signed overflow follow RISC-V M-extension result rules;
  they do not trap.
- If an instruction has insufficient gas, `Trap::OutOfGas` wins and no VM state
  changes.
- After gas is charged, any non-gas trap leaves registers, memory, and `pc`
  unchanged except for the gas debit. This includes invalid instructions,
  misaligned accesses, and bad jump targets. `ecall` host results follow the
  Host Boundary rules below.

Trap classes:

- `InvalidInstruction`
- `BadPc`
- `BadMemory`
- `MisalignedAccess`
- `OutOfGas`
- `HostTrap`

Every trap must be deterministic and independent of host build mode.

## Gas Model

Milestone 1 uses deterministic instruction gas:

- Each successfully attempted instruction consumes at least 1 gas.
- Gas is checked and charged before the instruction mutates architectural state.
- M-extension instructions may have a higher fixed cost, but the cost table must
  be constant and test-covered.
- `ecall` charges the base instruction cost before entering the host callback.
  Future host syscall gas is accounted separately by the host bridge.
- The gas schedule is versioned as part of `groundhog-rv32im-ilp32-v1`, even
  while the VM is experimental.

The gas model is a meter, not a token burn mechanism. It limits execution and
produces deterministic accounting. Native token supply, NNInt deltas, and
storage conflicts remain host semantics.

## Host Boundary

Milestone 1 exposes a test-only host interface for `ecall`:

```cpp
class Host {
public:
  virtual ~Host() = default;
  virtual HostResult ecall(MachineView view,
                           const EcallArgs &args) noexcept = 0;
};
```

The test-only register convention is:

- `a7` (`x17`): syscall number.
- `a0`-`a6` (`x10`-`x16`): seven word arguments.
- `a0`: return value on `HostResult::Continue`.

`HostResult::Continue(value)` writes `value` to `a0` and resumes at the next
instruction. `HostResult::Halt(code)` produces normal VM termination.
`HostResult::Trap` produces `Trap::HostTrap`.

`Machine::step()` reports `StepResult::Ecall(EcallArgs)` after charging gas and
advancing `pc` past the `ecall`. The caller may complete it manually for tests.
`Machine::run(Host &)` drives that loop, calls `Host::ecall()`, and applies the
host result.

The host may read and write VM memory through a bounded view. The first milestone
uses mock hosts only. A production bridge, if built later, must route through the
same parsing, alias-guard, tracing, and gas machinery used by the WASM syscall
path.

The seven-register test convention is not sufficient for production
`SystemCallHandler` parity. The current WASM boundary passes a syscall number
and ten `uint64_t` argument words through `syscall_entry`. Milestone 2 must
therefore add a production-shaped memory marshalling convention, such as an
`ecall` frame pointer in guest memory containing the syscall number and ten
64-bit argument slots, rather than trying to squeeze the full syscall ABI into
RV32 argument registers.

## C++ API Shape

The implementation should live under `hogd/hogexec/runtime/rv32/` and expose a
small public surface:

- `Program`: immutable validated instruction image.
- `Program::validate(std::span<const uint8_t>)`: validates length, alignment,
  instruction encodings, and profile restrictions.
- `Machine`: registers, PC, memory, gas, and run/step methods.
- `EcallArgs` and `HostResult`: explicit test-only `ecall` boundary types.
- `Host`: abstract `ecall` callback for tests and future runtime integration.
- `Trap`: deterministic trap enum.
- `StepResult`: `Continue`, `Ecall`, `Halted`, or `Trap`.

The VM library should not depend on HogDB, protocol deployment types, WASM
runtime classes, or SDK crates. Tests may use GoogleTest fixtures and local mock
hosts.

## C++ Implementation Style

The VM should feel like it belongs in this repository. Treat `hogd/mempool` as
the local model for clear production C++ and `x23` as the model for carefully
hand-written performance-sensitive C++23.

Guidelines for the VM core:

- Follow the repository formatting and naming conventions: `PascalCase` types,
  `snake_case` functions, `kCamelCase` constants, and trailing underscores for
  members.
- Use fixed-width integer types, `std::span`, `std::array`, `std::optional`, and
  `std::expected` where they make ownership and fallibility explicit.
- Keep hot-path code direct and predictable. Prefer small value types, local
  helpers in anonymous namespaces, and simple branch structure over generalized
  abstraction.
- Use `constexpr inline` tables and helpers where they make decode and gas rules
  explicit without runtime setup.
- Use RAII for owned buffers and host resources; do not expose raw ownership
  transfer in the public VM API.
- Do not use exceptions for ordinary VM traps or guest errors. Reserve hard
  assertions for host logic bugs and impossible internal states.
- Comment the reason for non-obvious VM rules, especially determinism, trap,
  gas, and RISC-V edge-case behavior. Avoid boilerplate comments that restate
  the code.
- Keep dependencies narrow. The VM core should need standard C++ and small
  `hogd/base` utilities at most; any dependency on broader runtime, storage,
  RPC, or SDK code belongs in later integration layers.

## Determinism Requirements

- No undefined behavior in instruction decode, sign extension, shifts, division,
  address arithmetic, or memory access.
- Use fixed-width integer types throughout.
- Use checked arithmetic for host indexes and bounds checks.
- Do not reinterpret guest memory as host structs.
- Do not depend on host endianness.
- Do not use exceptions for normal VM traps.
- Keep debug logging out of consensus-visible behavior.

Specific RISC-V edge cases must be tested:

- Shift amounts use only the low 5 bits for both register and immediate forms.
  Never shift a 32-bit C++ value by 32 or more.
- M-extension divide and remainder must be implemented without C++ `/` or `%`
  on undefined cases:
  `DIV/0 -> -1`, `DIVU/0 -> 0xffffffff`, `REM/0 -> dividend`,
  `REMU/0 -> dividend`, `DIV(INT32_MIN, -1) -> INT32_MIN`, and
  `REM(INT32_MIN, -1) -> 0`.
- `MULH`, `MULHSU`, and `MULHU` must use explicit 64-bit intermediates with the
  correct signedness.
- Loads must sign- or zero-extend exactly according to the instruction.
- Branch and jump target arithmetic must wrap as RV32 arithmetic, then apply the
  profile's alignment and program-image bounds checks before changing `pc`.

## Groundhog Fit

The guest VM does not implement Groundhog's economic or storage semantics.
Groundhog-specific behavior remains in the host:

- NNInt serialization, conflict narrowing, nonnegative bounds, and set/add
  merging remain HogDB/protocol semantics.
- Native token transfers remain paired host-side balance deltas.
- Gas remains metered execution budget, not a burn rule.
- Proposal, validation, and viewing modes remain `SystemCallHandler` behavior.
- Contract calls, auth-token handling, return buffers, events, and traces remain
  syscall/runtime behavior.

This keeps the VM small: it only supplies deterministic compute, memory, and a
host boundary.

## Implementation Milestones

### Milestone 1: VM Library

- Add the C++ RV32 library and unit tests.
- Implement validation, decode, execution, traps, gas, and mock `ecall`.
- Add focused microbenchmarks if the local benchmark target is available.
- Do not modify protocol or runtime dispatch.

### Milestone 2: Host Bridge Tests

- Add a test-only bridge that maps RV32 `ecall` arguments to a mock syscall
  table shaped like the current WASM syscall boundary.
- Prove pointer/length parsing and memory aliasing can use the same invariants
  as the WASM path.

### Milestone 3: Experimental RuntimeContext

- Add an in-tree experimental `RuntimeContext` implementation behind tests or a
  compile-time flag.
- Keep it out of deployed contract selection.
- Compare invoke status, host-visible state mutations, and traces against
  equivalent WASM test contracts. Gas is compared for stability within a runtime,
  not for raw numeric equality between WASM and RV32.

### Milestone 4: SDK Toolchain Spike

- Add a separate SDK experiment for `riscv32im-unknown-none-elf`.
- Replace WASM export-name entrypoints with an RV32 dispatch table or fixed
  entry stub.
- Keep Rust as the first language target.

### Milestone 5: Protocol Exposure

- Consider `ContractType::kRv32` only after conformance, fuzzing, benchmarks,
  and SDK ergonomics are acceptable.
- Version the artifact header, VM profile, memory limit, and gas schedule
  explicitly.

## Milestone Status

Current status as of 2026-05-27: Milestones 1 through 4 are complete for the
experimental, test-only scope. The next decision point is M5 readiness and
whether to harden RV32 toward protocol exposure.

### Milestone 1 Status: Complete

- RV32IM validation, decode, execution, traps, gas, and mock `ecall` are
  implemented under `hogd/hogexec/runtime/rv32/`.
- The VM core remains isolated from production WASM runtime state and protocol
  dispatch.
- Unit tests cover instruction families, gas-before-mutation ordering,
  deterministic trap behavior, host completion, and representative loop
  execution.

### Milestone 2 Status: Complete

- The test-only syscall bridge reuses the production-shaped `syscall_entry`
  boundary through an RV32 memory frame containing the syscall number and ten
  `uint64_t` argument words.
- Host syscall gas is reported through `HostResult` and debited after ECALL base
  gas but before committing host-visible effects.
- Out-of-gas, guest-memory parser faults, and fatal host traps are atomic with
  respect to guest memory writeback.
- Fatal host panic codes are named with `HostFnErrorCode` and propagated without
  collapsing known consensus outcomes.

### Milestone 3 Status: Complete

- The experimental `rv32::RuntimeContext` is test-only and remains restricted to
  `ContractType::kMockTest`; no production contract selection path was added.
- RV32 invokes run through the shared `SystemCallHandler` and real
  `syscall_entry`, preserving the production syscall parser, tracing, frame, and
  gas-normalization machinery.
- RV32/WASM differential tests now compare real host-visible success behavior:
  invalid-syscall traces, SHA256 plus Return output, storage writes, events,
  return buffers, transaction stats, and trace bodies.
- Runtime outcome mapping mirrors WASM for host panics:
  `OutOfGas`, `Return`, `Exit`, `Unrecoverable`, `ValidationFailure`, and
  `Abandon`; unknown host panic codes map to `Unrecoverable`.
- Runtime branch tests pin guest traps, out-of-gas normalization, zero halt as
  success, nonzero halt as deterministic failure, and same-input determinism.
- Debug and ASan RV32 CTest filters pass for the milestone test surface.

### Milestone 4 Status: Complete

Milestone 4 started from the test-only runtime proof and answered the
SDK/toolchain viability question:

- A committed Rust `riscv32im-unknown-none-elf` SHA256 fixture now lowers to an
  RV32 artifact and runs through the unchanged VM with WASM parity checks.
- The experimental artifact format is versioned and validated before runtime
  construction, including VM profile, ABI pins, gas schedule metadata, code,
  memory, and dispatch-table digests.
- Runtime dispatch now resolves method IDs through the artifact dispatch table
  to a validated initial PC. Unknown selectors fail deterministically with zero
  gas before VM construction.
- This intentionally chooses artifact-visible runtime dispatch over the earlier
  guest-side fixed entry stub option, because it keeps the method set
  auditable before VM execution and makes dispatch misses runtime-deterministic.
- A compiled Rust fixture now contains distinct RV32 entry PCs for the SDK
  `Sha256Wrapper::sha256` and `Sha256Wrapper::sha256_repeated` method IDs, and
  RV32/WASM parity tests invoke both methods plus an unknown method.
- Dispatch remains experimental and `ContractType::kMockTest`-only. The
  guest-side fixed entry stub plus invocation frame remains a documented
  alternative, but is not the current M4 dispatch direction.
- A C++ ownership review concluded that the VM core is small, explicit, and
  maintainable enough to keep owning, while the host/runtime/artifact edges need
  the M5 hardening listed below. See
  `2026-05-27-rv32-cpp-ownership-review.md`.

### Deferred Hardening Before Milestone 5

These are not blockers for Milestone 4, but they should be closed before any
protocol exposure:

- Split overloaded `HostResult.value` into typed payloads or add an explicit
  trap-code field so return values, halt codes, and host trap codes cannot be
  confused by status discipline alone.
- Classify bridge ABI faults caused by guest input, such as invalid bridge
  version, misaligned frame pointer, unreadable frame, or code-zero host traps,
  as deterministic guest failures rather than fatal host failures.
- Fold syscall gas costs into the versioned gas-schedule digest so total RV32
  contract cost cannot change silently when syscall costs change.
- Add differential WASM-vs-RV32 error contracts for validation failure,
  abandon, deterministic guest failures, and relevant host trap classes.
- Continue planned bridge cleanup: replace scratch-copy memory writeback with a
  live mutable memory span if the performance/determinism tradeoff holds up.

## Review Gates

Before moving beyond Milestone 1:

- All instruction semantics and trap tests pass.
- Fuzzing covers decode and memory access; differential checks against a
  reference RISC-V model should be added when practical.
- The VM library has no dependency on production WASM runtime state.
- A reviewer confirms no protocol-visible behavior changed.
- Benchmarks show whether interpreter overhead is plausible for Groundhog-shaped
  workloads.

Before protocol exposure:

- Cross-platform determinism tests pass on supported validator platforms.
- RV32 and WASM conformance tests agree on host-visible outcomes for shared test
  contracts: invoke status, trap class, storage effects, events, return values,
  and traces. Raw gas equality across runtimes is not required.
- Gas schedule is stable, documented, and versioned.
- Artifact validation rejects malformed or unsupported programs deterministically.
