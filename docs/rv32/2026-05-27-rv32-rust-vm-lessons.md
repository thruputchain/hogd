# RV32 Rust VM Prior Art Lessons

Companion note for the experimental RV32 VM effort. This reviews the two local
Rust VM checkouts under `~/code/gh/rv32`:

- `ckb-vm`: Nervos CKB's production RISC-V scripting VM.
- `polkavm`: Parity's RISC-V-based user-level VM.

This is not a recommendation to adopt either VM. Groundhog's implementation
should stay tight, C++, in-tree, and scoped to `groundhog-rv32im-ilp32-v1`.
The value here is to steal design pressure, test ideas, ABI discipline, and
failure lessons from more mature systems.

## Executive Summary

The most useful lesson from both projects is that a VM becomes difficult at the
artifact and boundary layers before the arithmetic instruction set becomes
difficult.

CKB-VM's hard-won lesson is compatibility: once VM behavior is chain-visible,
every bug, stack convention, ELF loader edge case, and memory boundary rule
becomes something you either preserve or explicitly version away. Its codebase is
full of version gates, regression programs, and fuzz/differential tests because
that is what production consensus VMs accumulate.

PolkaVM's useful lesson is product shape: it treats raw compiler output as an
input to a more explicit VM artifact, with imports, exports, jump metadata, memory
map, gas mode, and host-call linkage resolved before execution. That is directly
relevant to M4. Even if Groundhog keeps the runtime much simpler, M4 should avoid
making raw ELF or symbol lookup the consensus artifact.

For Groundhog M4, the strongest recommendation is:

- compile Rust to a RISC-V ELF as a build intermediate;
- lower it into a Groundhog RV32 artifact with explicit profile, ABI, memory, gas
  schedule, dispatch, and syscall-frame metadata;
- run a single deterministic entry stub rather than runtime-resolving named
  exports;
- keep host interaction as explicit memory-resident frames.

## CKB-VM Lessons

### 1. Version VM Semantics From The First Protocol Artifact

CKB-VM carries `VERSION0`, `VERSION1`, and `VERSION2` in its machine layer.
Version gates preserve or fix behavior around memory boundary reads, stack
layout, `jalr`, macro-op fusion, and other compatibility issues.

Groundhog should not emulate old bugs before it has old bugs. But the first
protocol-visible RV32 artifact should include explicit versions for:

- VM profile: `groundhog-rv32im-ilp32-v1`;
- artifact format;
- guest ABI / entry convention;
- syscall frame ABI;
- gas schedule digest;
- memory limit.

This matches the current spec direction, but CKB-VM is a reminder that those
fields are not bureaucracy. They are the escape hatch when an edge case turns
out to be wrong after deployment.

### 2. Keep The Charge-Before-Execute Rule

CKB-VM's Rust runner decodes the instruction, computes cycles through an
instruction-cycle function, adds cycles, and only then executes the instruction.
That mirrors Groundhog's current charge-before-mutation rule.

Keep this. It makes out-of-gas behavior easier to reason about, easier to test,
and easier to compare with a future reference oracle. Groundhog's stronger rule
that insufficient gas causes no architectural mutation is exactly the right
consensus shape.

### 3. `pc` / `next_pc` Separation Is A Useful Mental Model

CKB-VM separates the current PC from a staged next PC and commits explicitly.
Groundhog's implementation does not need to copy the trait-heavy shape, but the
mental model is valuable:

- fetch/charge from current PC;
- compute target or next PC;
- validate target before committing;
- commit only after the instruction has fully succeeded.

This is especially useful around M4 call stubs, dispatch misses, and `ecall`
resume behavior.

### 4. Do Not Inherit A POSIX-Like Program Loader By Accident

CKB-VM loads ELF program headers into memory and initializes an argc/argv stack.
That makes sense for CKB's scripting model, but it is not Groundhog's natural
contract model.

For M4, treat ELF as SDK input, not runtime truth. The build tool can parse ELF,
collect text/data/rodata, resolve relocations, and produce a smaller Groundhog
artifact. The runtime should not need to understand general ELF, argv, dynamic
segments, section flags, or linker ceremony.

### 5. Page Flags And Dirty Tracking Are Useful, But Not As VM Core Complexity

CKB-VM has multiple memory backends and page flags for executable/writable/dirty
state. Groundhog deliberately has separate immutable code and flat data memory,
so most of that complexity should stay out.

The part worth borrowing is diagnostic/accounting shape:

- dirty-page or dirty-range tracking can make tests cheaper;
- write-set tracing can help compare RV32 and WASM host-visible effects;
- a memory view should keep using explicit little-endian reads/writes, not host
  struct reinterpretation.

This belongs in testing and integration helpers first, not in the hot VM core.

### 6. Production VMs Need A Bug Graveyard

CKB-VM's test programs include many named historical edge cases: boundary reads,
misaligned jumps, `jalr` behavior, malformed ELF inputs, division, atomics,
macro-op bugs, and memory crashes. Its fuzz targets also compare ISA behavior
against Spike for selected extensions.

Groundhog should build the same kind of corpus while the surface is still small:

- imported RV32I/RV32M riscv-tests;
- handwritten regression programs for every trap/rollback/gas edge case;
- malformed artifact tests once M4 has an artifact format;
- decode and memory fuzzers;
- differential checks against Spike or sail-riscv before protocol exposure.

The key inspiration is not CKB's exact tests; it is the habit of turning every
found VM bug into a permanent named specimen.

## PolkaVM Lessons

### 1. The Artifact Layer Is The Main Inspiration

PolkaVM does not treat raw RISC-V ELF as the execution artifact. It has a parsed
program blob with code, ro/rw data, imports, exports, jump table, bitmask, memory
map, gas mode, and validation metadata.

Groundhog should not copy the PolkaVM bytecode format or reduced register ISA.
But M4 should copy the principle: the SDK emits a Groundhog artifact that says
exactly what the runtime needs and nothing more.

A plausible M4 artifact header should include:

- magic and artifact-format version;
- VM profile id;
- gas schedule version and digest;
- memory size;
- initial memory image hash/sections;
- code hash/size;
- entry ABI version;
- syscall frame ABI version;
- dispatch table offset/length or method-selector table;
- optional SDK/build metadata outside the consensus hash.

### 2. Resolve Entry And Host Linkage Before Execution

PolkaVM's linker pre-resolves imports and exports into an `InstancePre` before
calls run. Host calls then surface as `Ecalli(hostcall_number)` interrupts, and
the outer loop dispatches to the resolved host function table.

Groundhog already has the lower-level version of this with `Machine::step()`
returning `StepResult::Ecall` and `Machine::run(Host &)`. For M4, keep the same
shape:

- the VM reports a host boundary;
- the host bridge decides what that means;
- the VM resumes only with an explicit host result.

Do not add guest-visible dynamic imports or string-symbol host lookup. The SDK
can have friendly names, but the artifact should lower them into a fixed syscall
frame convention.

### 3. Typed Host Functions Are A Good SDK/Test Harness Idea

PolkaVM's typed linker is pleasant: host functions are registered with typed
Rust callbacks and marshalled through VM registers. Groundhog should not put that
machinery in the consensus runtime, but an M4 SDK test harness could use the same
idea:

- typed Rust wrappers for contract entry calls;
- typed Rust wrappers for host syscalls;
- deterministic lowering into the raw RV32 memory frame ABI;
- tests that assert the raw frames are exactly what the VM sees.

That gives developer ergonomics without making the VM itself know about Rust
types.

### 4. A Single Return Path Is Worth Designing Explicitly

PolkaVM treats a jump to `0xffff0000` as return-to-host. Groundhog should not use
that exact sentinel because standard RV32 plus `ecall` is cleaner for our
profile. But the design lesson is real: there should be one boring way for a
guest invocation to finish.

For M4, prefer an explicit return syscall or fixed return frame over a mixture of
fallthrough, bad-PC termination, named-export returns, and special host traps.
Normal contract return should be as unambiguous as `HostResult::Halt(0)` is in
the current test runtime.

### 5. Memory Map Discipline Matters Even If Groundhog Keeps One Page

PolkaVM has a formal memory map for rodata, rwdata, heap, stack, and aux data.
Groundhog's one-page flat memory is intentionally simpler, but M4 still needs an
SDK-level layout contract:

- where the stack starts;
- whether there is a reserved call frame area;
- where syscall frames are built;
- whether the SDK allocator owns a heap range;
- where immutable data from the ELF lands;
- what zero-initialized data means.

The runtime can still just see flat bytes. The SDK and artifact builder should
own the structure.

### 6. Crosschecking Is Valuable, But Multiple Engines Are A Trap

PolkaVM can crosscheck compiled execution against the interpreter. CKB-VM has
both Rust and assembly modes, and its README explicitly warns that Rust mode is
development-oriented while ASM mode is production.

Groundhog's current "one C++ interpreter" stance is healthier for consensus.
If a faster backend ever appears, it should be non-authoritative until it
crosschecks against the interpreter at the same host-boundary granularity:
interrupt kind, PC, registers, memory effects, gas, and host transcript.

## M4 Recommendations

### R1. Define A Groundhog RV32 Artifact, Not "Deploy An ELF"

Use `riscv32im-unknown-none-elf` as an SDK compilation target, then convert ELF
into a compact Groundhog artifact. Keep general ELF parsing out of
`hogd/hogexec/runtime/rv32`.

The artifact should be deterministic to parse, small enough to audit, and strict
about unsupported features. A malformed artifact should fail validation before a
`Machine` exists.

### R2. Use A Fixed Entry Stub With A Memory-Resident Call Frame

M3 currently treats method id `0` as the only RV32 entry. M4 should replace that
with a stable entry ABI, not with WASM-style export names.

A good shape:

- PC starts at a fixed entry stub;
- `a0` points to an invocation frame in guest memory;
- the frame contains method selector, argument buffer pointers/lengths, return
  buffer pointer/length, and ABI version;
- the SDK-generated stub dispatches to Rust functions through a static dispatch
  table;
- unknown method selectors produce a deterministic contract error.

This keeps the VM unaware of high-level method names while giving the SDK a
clean dispatch target.

### R3. Keep The Existing Syscall Frame Direction

The current M2/M3 bridge frame already points the right way: `a0` points at a
little-endian memory frame containing syscall number and ten 64-bit argument
slots, followed by a result footer.

For M4, make this an SDK ABI:

- generated syscall wrappers write the frame;
- `a7` carries the syscall bridge version or a small syscall ABI discriminator;
- `ecall` enters the host;
- the host writes the footer and returns through `a0`;
- out-of-gas and fatal host errors preserve the current rollback rules.

Do not squeeze production syscalls into RV32 argument registers.

### R4. Put ABI Versions In Both Code And Tests

Every SDK-generated artifact should make it obvious which ABI it expects. Tests
should reject mismatches deliberately:

- artifact says syscall ABI v1, VM supports v0 only;
- artifact says gas schedule digest X, runtime has digest Y;
- artifact memory size exceeds profile limit;
- dispatch table offset is out of bounds;
- call frame overlaps invalid memory;
- return buffer points outside memory.

This is CKB's version-compatibility lesson applied before protocol exposure.

### R5. Build A Small Prior-Art Test Corpus

Before M4 grows too much SDK surface, import or recreate:

- RV32I and RV32M ISA tests from the riscv-tests corpus;
- CKB-style regression tests for boundary reads/writes, bad jumps, malformed
  images, stack alignment, and division edge cases;
- PolkaVM-style artifact parser fuzzing once the artifact format exists;
- RV32-vs-WASM conformance tests at the host-visible result layer.

The fastest path to confidence is not a giant general-purpose VM; it is a small
VM surrounded by mean little tests.

## What Not To Copy

- Do not copy CKB-VM's broad ISA surface: atomics, compressed instructions,
  bitmanip, macro-op fusion, RV64, or assembly interpreters.
- Do not copy CKB-VM's POSIX-ish ELF/argv runtime model.
- Do not copy PolkaVM's reduced register set or custom VM instruction set.
- Do not copy PolkaVM's compiler/sandbox architecture for M4.
- Do not add dynamic imports, symbol lookup, or host function registration to the
  consensus runtime.
- Do not add page permissions or dynamic paging unless a later profile truly
  needs them.

## Source Files Consulted

CKB-VM:

- `/Users/markmayo/code/gh/rv32/ckb-vm/README.md`
- `/Users/markmayo/code/gh/rv32/ckb-vm/src/machine/mod.rs`
- `/Users/markmayo/code/gh/rv32/ckb-vm/src/memory/mod.rs`
- `/Users/markmayo/code/gh/rv32/ckb-vm/src/memory/flat.rs`
- `/Users/markmayo/code/gh/rv32/ckb-vm/src/cost_model.rs`
- `/Users/markmayo/code/gh/rv32/ckb-vm/src/syscalls/mod.rs`
- `/Users/markmayo/code/gh/rv32/ckb-vm/src/decoder.rs`
- `/Users/markmayo/code/gh/rv32/ckb-vm/fuzz/fuzz_targets/`
- `/Users/markmayo/code/gh/rv32/ckb-vm/tests/programs/`

PolkaVM:

- `/Users/markmayo/code/gh/rv32/polkavm/README.md`
- `/Users/markmayo/code/gh/rv32/polkavm/crates/polkavm/src/interpreter.rs`
- `/Users/markmayo/code/gh/rv32/polkavm/crates/polkavm/src/api.rs`
- `/Users/markmayo/code/gh/rv32/polkavm/crates/polkavm/src/linker.rs`
- `/Users/markmayo/code/gh/rv32/polkavm/crates/polkavm/src/gas.rs`
- `/Users/markmayo/code/gh/rv32/polkavm/crates/polkavm-common/src/abi.rs`
- `/Users/markmayo/code/gh/rv32/polkavm/crates/polkavm-common/src/program.rs`
- `/Users/markmayo/code/gh/rv32/polkavm/crates/polkavm/src/tests_riscv.rs`
- `/Users/markmayo/code/gh/rv32/polkavm/fuzz/fuzz_targets/`
