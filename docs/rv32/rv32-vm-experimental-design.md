# Experimental RV32 VM Design

## Purpose

Groundhog currently executes deployed user contracts through WASM. The RV32 work
is an in-tree C++ interpreter experiment: can we own a small deterministic VM
instead of inheriting all execution behavior from a vendored engine?

WASM remains production. RV32 is isolated behind `ContractType::kMockTest`.

A separate product note on optional ZK implications lives outside this repo at
`~/docs/groundhog-product/wip/rv32-zk-optionality.md`.

## Current Implementation

The implemented profile is `groundhog-rv32im-ilp32-v1`:

- ISA: RV32I plus M.
- Unsupported: compressed instructions, atomics, floats, vectors, CSRs,
  privileged instructions, threads, JIT/AOT.
- Endianness: little-endian.
- Memory: one bounded guest data memory, currently 64 KiB max.
- Code: separate immutable `Program` image; guest memory is not executable.
- Entry: runtime supplies an initial PC. Artifact dispatch maps method ID to PC.
- Host boundary: `ecall` only.

Important files:

- `program.{h,cc}` validates bytes and decodes once into `DecodedInstruction`.
- `machine.{h,cc}` owns registers, PC, memory, gas, traps, and execution.
- `host.h` defines `Host`, `MachineView`, `HostResult`, traps, and step results.
- `syscall_bridge.{h,cc}` adapts RV32 `ecall` frames to `syscall_entry`.
- `artifact.{h,cc}` parses the strict artifact format.
- `dispatch.{h,cc}` validates artifact method dispatch tables.
- `runtime_context.{h,cc}` is the test-only parity runtime.

## Execution Rules

- `x0` is always zero.
- `sp` starts at memory size rounded down to 16 bytes.
- Fetch requires a valid 4-byte-aligned PC in the immutable `Program`.
- Bad fetch traps before gas.
- Valid fetched instructions charge gas before semantic checks or mutation.
- Insufficient gas wins and leaves architectural state unchanged.
- Loads/stores require full bounds; any alignment is legal (byte-wise
  little-endian, as in WASM and RISC-V Zicclsm).
- Bad jump targets trap before writing link registers or changing PC.
- Division by zero and signed overflow follow RV32M rules, not C++ UB.
- Running off the code image is `Trap::kBadPc`, not success.
- Normal guest completion is a host halt result from an `ecall`.

## Gas

Instruction gas is a fixed constexpr table in `program.h`:

- base RV32I instructions: `1`
- multiply ops: `3`
- divide/remainder ops: `8`

`kGasScheduleDigest` now covers both RV32 instruction costs and the live host
syscall costs from `gas_cost_table.h`. `kGasTableVersion` is `2` after admitting
`EBREAK` as a deterministic breakpoint trap; protocol activation must treat
digest-affecting changes as version bumps.

`ecall` charges instruction gas first. Reported host gas is debited in
`Machine::complete_ecall` before committing `a0`, halt state, trap state, or
host memory effects.

## Host Boundary

The bridge ABI is register-native:

- `a0`: syscall number.
- `a1`–`a7`: up to seven syscall argument words.
- Each RV32 register is forwarded as one `uint32_t` syscall argument slot,
  matching the shared wasm32 syscall ABI. The shared parser interprets signed
  values from their two's complement bit pattern, uses `UINT32_MAX` as the
  optional-pointer null sentinel, and bounds-checks guest pointers and lengths
  against guest memory.
- **ABI constraint on syscall signatures:** every register-passed argument must
  be representable in one 32-bit slot — a guest pointer/length, a signed or
  unsigned 32-bit scalar, or an enum. A scalar that needs the full 64-bit range
  must travel through guest memory by pointer. `wrap_outer` enforces the
  `<= 7` argument-word count; `SyscallArgParams<T>` and `narrow_slot` enforce
  the `<= 32`-bit by-value scalar shape. Wider values such as `BlockHeight`
  travel through guest memory by pointer.
- Result: the syscall return code is delivered in `a0`; output buffers and
  lengths are written into guest memory through the pointers the guest supplied.
  There is no result frame or footer.
- There is no per-call version register: the syscall ABI version is validated at
  artifact load time (all eight registers are needed for the widest syscalls, so
  no spare register remains).

The reduced argument count (the invoke syscall packs `entrypoint_id`,
`gas_limit`, and `flags` into `CopyableInvokeInputs`, and its output lengths into
`CopyableInvokeOutputs`, both passed by pointer) is what lets every syscall fit
RISC-V's argument registers natively. The bridge reuses the shared
`syscall_entry` path, so parser, aliasing, tracing, gas, and output behavior
remain aligned with WASM.

Atomicity policy:

- success, `Return`, and `Exit` write back scratch memory;
- parser/output fault returns the fault code in `a0` without writeback;
- OOG, host throws, and fatal/unknown host panics do not write back scratch
  memory;
- host OOG and over-budget host gas roll guest memory back to the pre-host
  ECALL snapshot.

Host traps carry typed payloads:

- `HostTrapKind::kDeterministic` for guest-caused bridge ABI faults;
- `HostTrapKind::kFatal` for genuine host/internal failures.

## Artifact Format

The current artifact format is v3:

- magic: `GHRV32A1`
- header bytes: `172`
- VM profile digest
- entry ABI version: `1`
- syscall ABI version: `1` (register-native: args in `a0`–`a7`, no frame/footer)
- gas schedule version and digest
- memory size, code size, initial memory size
- offsets for code, dispatch table, and initial memory
- SHA256 commitments for code, initial memory, and dispatch table

Parser rules:

- little-endian explicit reads only;
- no host struct overlays;
- exact canonical layout: header, code, dispatch, memory, no gaps/trailing bytes;
- full initial memory image required;
- dispatch table required, sorted by method ID, reserved bytes zero;
- dispatch PCs must be valid `Program` PCs.

The artifact parser does not parse ELF. ELF is fixture/lowerer input only.

## Dispatch

M4 chose runtime-side artifact dispatch:

- the artifact contains method ID to PC entries;
- runtime validates the table before VM construction;
- known methods run at the selected validated PC;
- unknown methods fail as deterministic misses with zero gas.

This intentionally replaced the earlier guest-side fixed-entry stub idea. The
guest-side shape may still be useful for a future SDK, but it is not the current
dispatch mechanism.

## Rust Fixture And Lowering

The committed Rust fixture proves that real `no_std` Rust compiled to
`riscv32im-unknown-none-elf` can run through the unchanged VM and match WASM on
host-visible behavior.

The VM is Harvard-style:

- PC indexes immutable `Program` code.
- Loads/stores address guest data memory.
- Code bytes are not executable guest memory.

The fixture layout makes link-time virtual addresses match PC-space:

- `.text` starts at virtual address `0`.
- `.rodata`, `.data`, `.bss`, buffers, and stack live above `.text`
  inside the 64 KiB memory limit.
- Low guest-memory bytes corresponding to `.text` are dead space.
- Initial memory is a full 64 KiB image, not sparse sections.

The fixture lowerer must fail loudly on unsupported compiler output:

- `.text` starts at `0`;
- no unresolved relocations;
- no unsupported allocated sections;
- no executable section other than `.text`;
- no overlapping data sections;
- no data/object symbols in `.text`;
- dispatch entry symbols resolve to valid `.text` PCs;
- build metadata records relevant source/config hashes.

The Rust fixture uses the register-native RV32 bridge ABI: the guest loads the
syscall number into `a0` and arguments into `a1`–`a7`, executes `ecall`, and
reads the return code from `a0` (outputs are written through the guest pointers
it passed). Normal completion is the Return syscall; `_start` must not return.

## Milestone Status

- M1 VM core: complete.
  - RV32IM validation, decode, execution, traps, gas, and mock host calls.
- M2 host bridge: complete.
  - Shared `syscall_entry`, host gas debit, writeback gating, fault atomicity.
- M3 runtime parity: complete.
  - Test-only `RuntimeContext`, WASM/RV32 parity for traces, return buffers,
    storage writes, events, stats, and outcome mapping.
- M4 compiled Rust/artifact/dispatch: complete.
  - Committed Rust fixture, strict artifact parser, runtime dispatch table,
    multi-method compiled fixture with WASM parity.
- Early M5 hardening: partly complete.
  - Typed host payloads, deterministic bridge-fault classification, SHA256
    artifact commitments, gas digest including syscall costs, RV32/WASM
    differential error outcome tests, a first-pass conformance corpus, and an
    in-tree spec-model cross-check are in place.

## Remaining Before Protocol Exposure

- Cross-platform determinism runs on supported validator platforms.
- External-oracle differential testing with Spike, sail-riscv, or equivalent.
- Production SDK lowerer: method manifest, typed syscall wrappers, reproducible
  metadata, and enforceable `.text` data policy.
- Continued RV32I/RV32M conformance expansion as bugs surface.
- Fuzzing and sharper parser/dispatch error codes for triage.
- Activation rule: artifact verification must either use the consensus SHA256
  wrapper or carry an explicit equivalence check for direct OpenSSL SHA256.
- Activation rule: any digest-affecting gas schedule change bumps
  `kGasTableVersion`.

## Review Gates

Do not expose RV32 to protocol until:

- RV32 and WASM agree on host-visible outcomes for shared contracts.
- Guest traps, host traps, validation failure, abandon, and OOG are pinned.
- Artifact validation rejects malformed inputs deterministically.
- Cross-platform determinism is demonstrated.
- Conformance and fuzzing cover decode, memory, traps, and RV32I/RV32M semantics.
- External-oracle differential testing with Spike, sail-riscv, or equivalent is
  green.
