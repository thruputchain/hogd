# RV32 Runtime Design Map

## Purpose

`hogd/hogexec/runtime/rv32` implements the RV32IM contract runtime. It plugs
into the same executor-facing `Runtime` / `RuntimeContext` shape as the wasm
runtime, while keeping the portable ISA core free of wider `hogd` dependencies.

The runtime accepts lowered RV32 artifacts, validates their format and gas
schedule, prepares a decoded production representation, and executes contract
entrypoints against the shared host syscall layer.

## Layers

The portable ISA core is the lowest layer:

- `program.h` / `program.cc`: instruction definitions, gas costs, validation,
  and decoded immutable `Program` objects.
- `isa_ops.h`: pure RV32 arithmetic and decode helpers shared by engine
  implementations.
- `machine.h` / `machine.cc`: canonical per-instruction interpreter used as a
  reference engine and oracle adapter.
- `host.h`, `arch_trace.h`, `oracle.h`, `spec_model.h`: model-neutral host,
  tracing, and differential-test infrastructure.

The production/runtime layer is above that:

- `artifact.h` / `artifact.cc`: artifact header parsing, section validation,
  ABI/profile checks, and gas-schedule digest validation.
- `threaded_program.h` and `threaded_machine.h`: production direct-threaded
  interpreter, run lowering, fusion, and execution.
- `rv32_contract.h` / `rv32_contract.cc` and `runtime_context.*`: executor
  integration and cached prepared-contract storage.
- `syscall_bridge.*`: testable host bridge for the register-native syscall ABI.

## Execution Engines

There are intentionally multiple engines:

- `Machine` is the canonical, simple interpreter. It charges gas and executes
  one instruction at a time, which makes it easy to reason about and compare
  against external models.
- `ThreadedMachine` is the production engine. It lowers decoded instructions
  into compact executable slots, uses musttail direct dispatch, and can select
  fused handlers.
- `spec_model.h` is an independent hand-written test oracle. It deliberately
  does not share `isa_ops.h` with the engines.
- `spike_oracle.h` adapts an external Spike oracle when available; those tests
  are optional in local/CI runs.

When changing ISA semantics, update the canonical engine and production engine
together, then rely on the spec/corpus/oracle tests to keep them honest.

## Artifacts

RV32 contracts are deployed as artifacts, not raw instruction streams. The
artifact header pins the format version, VM profile, syscall ABI version,
memory size, section digests, gas table version, and gas schedule digest.

`Rv32Contract::create` parses the artifact and builds a
`ThreadedMachinePreInstantiation`. That preparation step rejects invalid code,
invalid memory sizing, and straight-line runs whose precharged gas would not fit
in the threaded slot representation.

## Gas And Traps

Instruction gas costs live in `program.h`. The artifact gas digest in
`artifact.h` folds those instruction costs together with the host syscall gas
schedule, so artifacts built against a different gas table are rejected.

`Machine` meters per instruction. `ThreadedMachine` meters straight-line runs:
the lowering step records the total gas for a run, and `enter_run()` charges it
once before executing the run body. Control-flow and host boundary instructions
are barriers. Loads and stores remain in straight-line runs for performance, so
a memory trap can fire after the run has already been charged past it;
`trap_bad_memory` refunds the run's unexecuted suffix so the trapped state
keeps exactly the instructions through the faulting one charged.

An unaffordable run still executes speculatively on a zero balance with the
shortfall recorded out-of-band; the deficit settles as `kOutOfGas` with zero
gas at the run's exit seam, before any effect can escape the frame. The seams
are exhaustive: `enter_run` at the next run head, `op_ecall` before the host
sees a syscall (a speculative syscall would have effects outside the frame),
and `trap_at` / `trap_bad_memory` at every trap. At most one run ever
executes in deficit (every head settles first), bounding the overdraft by
`kMaxRunGas` (the lowering's chunk cap; over-cap straight-line runs are split
at mid-run break carriers rather than rejected).

The invariant this buys: the consensus-visible pair (trap classification,
`gas_remaining`) matches the canonical `Machine` at every budget and on every
trap class -- every instruction through and including the trapping one is
charged, nothing after it, and all `Trap::kOutOfGas` paths saturate
`gas_remaining` to zero in the canonical engine, production threaded engine,
and independent spec model. This pair is consensus-visible because
`rv32_contract.cc` reports `gas_limit - gas_remaining` to the caller as the
frame's consumption, and a TryInvoke parent continues executing with the
remainder. NOT invariant (frame-local state a trap discards): the trap pc, a
speculative run's register/memory writes, and traced intermediate gas past
the OOG fence. `Rv32ThreadedTrapGas` pins engine parity for every trap class
-- including each fused handler's fault paths and every deficit seam -- and
`Rv32Threaded.outOfGasMidRunSettlesAtNextSeam` pins the speculative
semantics.

## Syscalls

The RV32 guest syscall ABI is register-native: the guest places the syscall
number in `a0`, up to seven argument words in `a1..a7`, and executes `ecall`.
The shared syscall parser interprets those words as pointers, lengths, scalars,
enums, and optional sentinels.

Production drives the `ecall` loop in `rv32_contract.cc` against live guest
memory for performance: syscall output spans point directly into the active
`ThreadedMachine` memory. Syscall handlers that need atomic output behavior must
stage internally before writing to those spans. `SyscallBridgeHost` is a
canonical-test seam for the same register ABI; it intentionally runs
`syscall_entry` against scratch memory so tests can inspect faults/OOG without
committing partial writes to `Machine` memory. Do not infer production output
rollback semantics from the bridge.

## Testing

The test strategy is deliberately redundant:

- `rv32_vm_test.cc` and `rv32_conformance_test.cc` cover canonical execution.
- `rv32_borrowed_conformance_test.cc` checks borrowed RISC-V-style vectors.
- `rv32_spec_model_crosscheck_test.cc` compares the independent spec model.
- `rv32_oracle_test.cc` and `rv32_spike_oracle_test.cc` cover oracle adapters.
- `rv32_threaded_test.cc` compares production lowering/execution against the
  canonical engine and exercises traced and untraced paths.
- `rv32_syscall_bridge_test.cc`, `rv32_syscall_entry_bridge_test.cc`,
  `rv32_runtime_context_test.cc`, and `rv32_contract_test.cc` cover ABI and
  executor integration.

For consensus-sensitive changes, prefer a narrow regression test near the
changed layer plus at least one cross-engine check.
