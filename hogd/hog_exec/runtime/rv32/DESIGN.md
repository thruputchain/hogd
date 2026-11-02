# RV32 runtime

This directory owns the RV32IM interpreter, artifact validation, executor
adapter, and their tests. Start with [rv32_contract.cc](rv32_contract.cc) for
production execution and [machine.cc](machine.cc) for instruction semantics.

## Production path

1. `Rv32Context::verify_contract` validates an attached artifact against the
   chain's configured code and memory limits. `Rv32Contract::create` parses it
   and builds a `ThreadedMachinePreInstantiation`: executable slots and a
   zero-padded initial memory image.
2. The prepared contract is retained by the transaction's `SystemCallHandler`
   or the block's [StorageSnapshotCache](../item/storage_snapshot_cache.cc).
   Loading an already-deployed contract uses the VM ceilings rather than the
   current admission limits, so lowering a chain limit does not invalidate
   existing deployments.
3. `Rv32Context::make_new_runtime` borrows that prepared contract.
   `Rv32Runtime` creates a machine per invocation, copies the initial memory,
   seeds the method ID, and drives execution through `start()` and
   `resume_from_ecall()`.
4. The invocation coroutine owns the machine while awaiting
   [syscall_entry](../syscall_entry.cc). It maps host errors and VM traps to
   executor outcomes and reports `gas_limit - gas_remaining` as consumption.

[make_hog_exec_context](../../precompiles/context.cc) selects this adapter for
`ContractType::kRv32`. The similarly named [runtime_context.h](runtime_context.h)
defines a **test-only** adapter: `rv32::RuntimeContext` runs the canonical
`Machine` under `kMockTest`. It is not the production context or cache.

## Instruction and memory model

The profile supports RV32I and M with fixed four-byte instructions and
little-endian data. Code is immutable and separate from guest data memory;
loads and stores cannot read or modify instructions. Unaligned data accesses
are allowed when the entire access is in bounds. Jump targets must be aligned
and within the code image. Falling off the image traps.

The decoders in [program.cc](program.cc) and
[threaded_program.h](threaded_program.h) define the accepted encodings.
Compressed, atomic, floating-point, vector, and general CSR/privileged
instructions are rejected. The admitted SYSTEM words are `ECALL`, `EBREAK`,
and canonical `UNIMP`; the latter two terminate with VM traps. `FENCE` is a
no-op. [isa_ops.h](isa_ops.h) handles arithmetic cases that need explicit
unsigned operations or special treatment of division overflow and zero.

[program.h](program.h) owns default limits and VM ceilings. Admission limits
come from network configuration. An artifact declares its data-memory size;
the stored image is copied at `initial_memory_start`, with zeros on both sides.
Zero-sized data memory is valid and every load/store against it faults.

## Artifacts and guest entry

[artifact.h](artifact.h) defines the wire constants and header layout;
[artifact.cc](artifact.cc) defines validation. The parser reads explicit
little-endian fields and requires exactly `header || code || initial_memory`,
with no gaps or trailing bytes. SHA256 commitments cover the stored code and
memory sections. ELF parsing belongs to the author toolchain.

The runtime enters at PC 0 with registers zeroed except for the method ID:
`a0` contains its low 32 bits and `a1` its high 32 bits. The guest initializes
`sp` and dispatches the method. There is no runtime method table or pre-entry
unknown-method check. SDK dispatchers handle unknown IDs in guest code, so
that path consumes gas; the runtime itself does not enforce a method set.
Successful completion uses the `Return` syscall; `_start` must not return or
fall through.

The producer and consumer are maintained in different repositories:

| Owner | Contract |
|---|---|
| SDK (`sdk_rs`) | Guest startup, method dispatch, and typed syscall wrappers |
| Tooling (`thruput-tools`, `crates/rv32/artifact` and `crates/rv32/lower`) | ELF validation and artifact writing; shared by the CLI and standalone lowerer |
| This directory | Artifact admission, preparation, and execution |

Guest dispatch removes the need for the lowerer to discover method symbols or
serialize method PCs. Contract specs remain author-tool outputs, separate from
the execution artifact. [build_risc.sh](../../../../built_contracts/build_risc.sh)
uses the pinned CLI to produce integration-test artifacts and metadata;
[built_contracts/CMakeLists.txt](../../../../built_contracts/CMakeLists.txt)
owns their build targets.

`kArtifactFormatFingerprint` records the format constants and validation
policies with an explicit encoding. A compile-time assertion pins the C++
value; `LoweredSdkArtifactFormatFingerprintMatchesRuntime` checks generated
lowerer metadata against `kAcceptedFormatFingerprints`. The fingerprint is a
compatibility check, not a substitute for parsing or instruction validation.
Format, entry ABI, syscall ABI, and gas schedule are separate versioned
contracts; update the relevant producer, consumer, fixtures, and checks together.

## Execution engines

| Implementation | Role |
|---|---|
| `Program` / `Machine` | Canonical decoder and per-instruction interpreter |
| `ThreadedProgram` / `ThreadedMachine` | Production decoder, lowering, and direct-threaded execution |
| `spec_model.h` | Test model written independently of `isa_ops.h` |
| `spike_oracle.h` | Optional adapter to an external ISA implementation |

The canonical and production decoders classify instructions separately so
differential tests can catch decode disagreements. The spec model can still
share a misunderstanding with both engines; it does not replace external
conformance testing.

Production lowering builds one slot per instruction plus a fall-off sentinel.
It resolves static branch targets, divides straight-line execution into gas
runs, and fuses selected instruction pairs. A fused pair retains the tail's
standalone handler so a branch can enter it directly. Traced handlers preserve
individual instruction observations for differential tests.

Dispatch requires the `musttail` attribute. [musttail.h](musttail.h) rejects
unsupported compilers because ordinary recursive calls would grow the host
stack with guest execution. The `rv32_isa_core` target has no artifact, crypto,
protocol, or executor link dependencies; `rv32_isa_core_test` checks that
boundary. [CMakeLists.txt](CMakeLists.txt) defines these targets, while
[the parent CMake file](../CMakeLists.txt) owns tests that link the executor.

## Gas and traps

Instruction costs are in [program.h](program.h); host costs are in
[gas_cost_table.h](../gas_cost_table.h). The artifact gas digest covers both.
`kAcceptedGasSchedules` accepts explicit `(version, digest)` pairs, including
transitional declarations. **Every accepted artifact executes at this build's
current prices**, regardless of the declaration it carries. Validators must
agree on both prices and accepted declarations. Removing an accepted entry
requires accounting for stored artifacts as well as the producing toolchain.
The [calibration guide](gas_calibration/README.md) describes measurement and
schedule changes.

`Machine` charges each instruction before executing it. `ThreadedMachine`
charges a run at entry, including its terminating control-flow or host-boundary
instruction. Lowering splits long runs at `kMaxRunGas`; run length is not an
artifact rejection condition. Loads/stores remain within runs. A memory fault
refunds the unexecuted suffix of the charged run.

An unaffordable run executes speculatively with zero balance and a recorded
deficit. That deficit settles at the next run entry, before an `ecall` reaches
the host, or on a trap. A memory fault in the affordable prefix keeps its
memory-trap classification after refunding the suffix; otherwise exhaustion
wins. At most one run executes in deficit, bounded by `kMaxRunGas`.

Trap classification and remaining gas must match canonical execution, since a
`TryInvoke` parent retains the unused gas. VM out-of-gas traps leave zero gas.
Registers, memory, trap PC, and intermediate trace gas may differ after
speculative execution in a frame that will be discarded. The trap-gas and
deficit tests in [rv32_threaded_test.cc](rv32_threaded_test.cc) cover this
distinction, including fused instructions and split runs.

## Syscalls

At `ecall`, `a0` holds the syscall number and `a1..a7` hold up to seven 32-bit
argument words. Wider values pass through guest memory. The shared
[argument parser](../syscall_entry_outer.h) owns pointer, length, scalar,
enum, sentinel, and alias checks. Return codes go in `a0`; output buffers are
written through guest pointers.

Production passes live machine memory to `syscall_entry`. A handler requiring
atomic output must stage its writes before touching those spans. The test-only
[SyscallBridgeHost](syscall_bridge.h) uses scratch memory and conditional
writeback. Its rollback behavior does not describe production syscall output.
Production resolves host errors in `Rv32Runtime`; it does not pass
`HostStatus::kTrap` back into the threaded engine.

## Stack-usage diagnostic

Debug builds configured with `HOGD_RV32_STACK_USAGE=ON` (the default when
`ENABLE_TEST_FFI` is on) record the first and the lowest value the guest
writes to `sp` (x2). Each handler store goes through `write_rd` in
[threaded_machine.h](threaded_machine.h), which keeps the first `sp` store and
lowers the `ThreadedMachine`'s minimum on every one. The first store is the
stack top `_start` establishes and the stack grows down from it, so the
minimum is the deepest point the stack reached and their difference is the
invocation's stack usage; a per-write record sees a leaf frame that is pushed
and popped within one run. When an invocation finishes, `Rv32Runtime` appends
both values, with the contract address and method id, to its
`SystemCallHandler`; the test FFI returns those records through
`get_rv32_stack_usage()` ([test_bindings.h](../../../test_ffi/test_bindings.h)).
Nothing is logged and nothing in the executor reads the records back.

A guest that never writes `sp` is recorded as `kSpUnwritten` for both. A fused
`lui;addi` stores only the pair's final value on the untraced path, so the
initial value is the constant the pair materializes rather than the `lui`'s
higher intermediate, and the minimum cannot be raised by it.

[stack_usage.h](stack_usage.h) owns the flag and constants. The define is
global because it adds members to `ThreadedMachine` and `SystemCallHandler`,
and CMake applies it to the Debug configuration only, so optimized
interpreters never carry the per-write check. The canonical `Machine` and the
test-only adapters are not instrumented. `Rv32ThreadedStackUsage` in
[rv32_threaded_test.cc](rv32_threaded_test.cc) covers the record and
`Rv32StackUsageTest` in the test FFI covers the round trip.

## Verification

Run commands from the repository root, using a configured build with the
repository's toolchain and dependencies:

```sh
ctest --test-dir BUILD_DIR --output-on-failure -R '^rv32\.'
```

The test prefix covers ISA conformance, artifact rejection, cross-engine
execution, syscall bridges, runtime adapters, and calibration tests. Production
executor tests also live in the [parent runtime directory](..) and
[contract_tests](../../contract_tests); the prefix alone is not full executor
coverage. For semantic changes, add a focused regression and a cross-engine
check, then exercise the affected production path.

Coverage limits:

- Borrowed conformance cases retain upstream operand/result vectors; see the
  [license notice](riscv-tests-license.md). `rv32_upstream_artifact_test.cc`
  runs synthetic upstream-shaped artifacts through the canonical test adapter,
  not compiled upstream assembly through the production engine.
- Spike tests skip unless `RV32_ORACLE_BIN` names an executable. The adapter
  compares architectural traces without gas. Its relocated code mapping
  excludes `AUIPC`, `JALR`, linking `JAL`, and breakpoint/illegal-instruction
  trap cases, and requires zero initial registers and memory. The predicate
  `spike_supported()` and corpus construction define its scope; passing it
  does not establish conformance for arbitrary compiled contracts.
- Generated SDK smoke tests check the format fingerprint and an
  increment/commit/get round trip through `rv32::RuntimeContext`. Production
  integration is covered separately by `rv32_contract_test.cc` and the
  executor tests. The committed Rust fixture under `fixtures/` is a separate
  regression input with its own pinned regeneration toolchain.
- A test run on one platform does not establish cross-platform determinism.
  Compare supported validator builds when changing platform-sensitive behavior.

[Fuzzing](fuzz/README.md) exercises artifact validation/preparation and
canonical/threaded execution. Both targets run in the scheduled RV32 fuzz
workflow; they are optional in ordinary builds. Calibration procedures and
retained measurement evidence live under [gas_calibration/](gas_calibration/README.md).
