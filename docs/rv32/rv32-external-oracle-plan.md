# RV32 External-Oracle Differential Testing — Scope

This is the working plan for the one conformance layer the other docs keep
deferring: an **independent external oracle** (Spike or sail-riscv) run in a
differential harness against the C++ RV32 VM. It is a named gate in three
places — the Runtime Boundary Gate
(`rv32-conformance-and-performance.md`), the open gates in
`rv32-upstream-test-plan.md`, and the activation-hardening list in
`README.md` — and must be green before broad protocol activation.

RV32 has a `ContractType::kRv32` deployed-contract path. This gate is part of
activation hardening, not a dependency of the local runtime plumbing.

## What this adds over the existing five layers

| Layer | What it proves | Limit |
|---|---|---|
| `rv32_conformance_test.cc` | Hand-audited semantics, one rule per case | Only the rules we thought to write |
| `rv32_borrowed_conformance_test.cc` | Matches upstream riscv-tests operand/result vectors | Fixed vectors, no program-level interaction |
| `rv32_spec_model_crosscheck_test.cc` | Machine == our hand-written spec model on random programs | **Both can share the same bug** |
| `rv32_upstream_artifact_test.cc` | Runtime boundary on upstream-shaped artifacts | Synthetic, hand-encoded |
| `rv32_unsupported_profile_test.cc` | Out-of-profile encodings rejected at load | Rejection only, not execution |

The external oracle's unique value is the third row's limitation: our `Machine`
and our hand-written spec model are written by the same people from the same
reading of the spec, so a shared misunderstanding passes the crosscheck
silently. An **independent** implementation of the ISA (Spike or sail-riscv)
is the check on the checker. It is the only layer where a divergence implicates
*our reading of RISC-V*, not just an internal inconsistency.

## Substrate already in place (thread B)

`arch_trace.h` (core-only, `rv32_isa_core`) is the model-agnostic comparison
substrate this gate plugs into:

- `ArchState` — canonical observable state; `==` and `describe_divergence`
  report the first differing field (pc → x# → memory byte → hogd
  bookkeeping).
- `capture_arch_state(Machine)` — snapshot via public accessors.
- `trace_execution(Machine, Host)` → `ExecTrace` — per-step `ArchState`
  sequence; `describe_trace_divergence` reports the first diverging step.
- `RecordingHost` — record-only `Host`, no syscall layer, so the pure core can
  run ecall-bearing programs in the harness.

The in-tree crosscheck was migrated onto this substrate, so the external oracle
is "one more producer of an `ArchState` sequence." Everything below is about
standing up that producer.

## The impedance list — where our VM is not a stock RISC-V model

These are the differences a naive `diff` would trip over. The harness must
either configure them away on the oracle side, restrict the corpus, or exclude
the field from comparison. Getting this list right is most of the work.

1. **Modified-Harvard split (the big one).** At the implementation level this is
   strict Harvard: PC is an *index* into `Program`'s instruction array
   (`pc/4 < instruction_count`); data is a **separate** `std::vector<uint8_t>`.
   Two physical stores. Two hard guarantees: PC fetches only from `Program`
   (never data); stores never reach `Program` (code is immutable). Spike/Sail
   are von-Neumann with one unified store.
   - The lowerer reconciles the numbering: `.text` links at vaddr **0** (so a PC
     *is* a `.text` byte offset), data sections sit at their natural vaddrs
     **above** `.text` in the full 64 KiB image, and the low `[0, text_size)` of
     data memory is inert dead space. So code refs fall in `[0, text_size)` and
     data refs in `[text_size, 64 KiB)` — one consistent numbering across two
     stores. Ordinary `AUIPC` + pc-relative `.rodata` load therefore resolves to
     the same bytes on both sides, which makes **ELF a viable shared input**: an
     oracle linked `.text@0` over the same 64 KiB image agrees by construction.
   - Residual divergence is confined to a program that reads the *code region*
     `[0, text_size)` as data: a von-Neumann oracle returns the real `.text`
     bytes there, we return inert dead space. Correct compiler output never does
     this; the lowerer's strict `.text`-data policy (no data/object symbols in
     `.text`, etc.) keeps it that way, and the random corpus sidesteps it by
     separating code and data. The harness must not compare reads of the code
     region, and must not model code as readable/writable data.
2. **Gas.** No RISC-V equivalent. `Trap::kOutOfGas` has no mcause. Exclude
   `gas_remaining` from cross-oracle comparison; the oracle never stops on it.
3. **Memory size.** Max 64 KiB (`kMaxMemoryBytes`), zero-filled, `sp` (x2)
   init to `memory_bytes & ~0xF`. The oracle must be configured to the same
   size and the same initial register/sp/pc state, or the corpus must stay in
   bounds. Out-of-bounds is a `kBadMemory` trap for us; on the oracle it is an
   access fault only if the region is genuinely unmapped there.
4. **Restricted ISA.** No compressed, no CSR, no privileged, no FP. `EBREAK`
   is accepted only as a deterministic terminal breakpoint trap, not as full
   debug/CSR machinery. The oracle must run the same restricted subset; any
   reference program outside it is out of scope (and is already covered by
   `rv32_unsupported_profile_test.cc` as rejection).
5. **Traps stop us; they vector on real RISC-V.** On a fault our `Machine`
   *halts* (`StepResult::kTrap`); real RISC-V sets mepc/mcause/mtval and jumps
   to a handler. So post-trap architectural state diverges by construction.
   **Lockstep comparison must terminate at the first trap** — compare the
   trajectory up to the faulting instruction, assert both detected a fault of a
   compatible class at the same pc, and stop.
6. **ecall is our host boundary, not a trap.** See ecall-boundary handling
   below.

### Equivalence checklist (false-positive divergence sources)

Subtle config/semantic items that will manufacture spurious divergences if
missed — these are the ones worth pinning explicitly:

- **Misaligned access:** we *allow* unaligned data loads/stores (bounds-only
  checking). Spike traps by default; it must run with the `Zicclsm` ISA
  extension (`--isa=rv32im_zicclsm` -- the old `--misaligned` flag no longer
  exists) to match. Sail has a `misaligned` config knob. Mismatch here looks
  like "we succeeded, oracle trapped."
- **Division/remainder by zero & signed overflow:** we follow the RISC-V
  defined results (DIV/0 → −1, REM/0 → dividend, DIV overflow → INT_MIN,
  REM overflow → 0). Oracles agree — a *non*-divergence worth asserting so a
  future regression here is caught.
- **Control-flow target alignment:** we require pc%4==0; JALR clears bit 0 per
  spec, so a 2-aligned JALR/branch target traps `kBadPc` for us and raises
  mcause 0 (instruction-address-misaligned) on the oracle. This is a current
  trap-taxonomy choice (data accesses have no alignment requirement at all).
  Maps cleanly; just don't compare past it.
- **x0 writes, FENCE as no-op:** both match a stock oracle; note for
  completeness.

## Axis 1 — Sail vs Spike

| | sail-riscv | Spike (riscv-isa-sim) |
|---|---|---|
| Authority | **The** formal spec model | Reference simulator, de-facto golden model |
| Language / build | OCaml + opam toolchain | C++ (same as our VM), autoconf/make |
| Per-step state | C emulator can dump; rougher | `--log-commits` per-instruction commit log; or link `processor_t` and step programmatically |
| ecall steerability | Harder | Easy (programmatic step + inspect/inject, or custom tohost) |
| Integration cost | Higher | Lower |
| Best role | Periodic high-assurance conformance pass | Day-to-day lockstep harness |

**Recommendation: Spike for the lockstep harness, sail-riscv as a later
periodic high-assurance gate.** Spike is C++ like our VM, has the mature
commit-log lockstep pattern (riscv-dv uses it as the golden model against RTL),
and is steerable at the ecall boundary. The substrate is oracle-agnostic, so
adding Sail later is a second adapter behind the same interface, not a rewrite.
Neither should become a hard dependency of the core CI build (see phasing).

## Axis 2 — lockstep vs end-state

- **Lockstep:** compare `ArchState` after every retired instruction; pinpoints
  the first diverging step (what `describe_trace_divergence` already does).
  Best diagnostic. Cost is per-step plumbing on the oracle side — cheap with
  Spike's commit log.
- **End-state:** run both to completion, compare the final architectural subset
  (pc, regs, memory). Cheapest to wire; best fuzzing throughput; a divergence
  only says "the ends differ," not where.

**Recommendation: lockstep as the primary mode** (the substrate is built for
it and the diagnostic is worth it), with **end-state as the high-throughput
fuzzing mode** over the random corpus. Both consume the same `ArchState`.

## Axis 3 — trap ↔ mcause mapping

| Our `Trap` | RISC-V mcause | Notes |
|---|---|---|
| `kNone` | — | running |
| `kInvalidInstruction` | 2 (illegal instruction) | unreachable post-load-validation in practice |
| `kBadPc` (misaligned) | 0 (instr addr misaligned) | from JALR odd targets |
| `kBadPc` (out of image) | 1 (instr access fault) | also fires for control-transfer targets outside the program |
| `kBadMemory` (load) | 5 (load access fault) | no MMU; access-fault, not page-fault |
| `kBadMemory` (store) | 7 (store/AMO access fault) | " |
| `kOutOfGas` | **none** | hogd-only; exclude, oracle keeps running |
| `kHostTrap` | **none** | result of our host's classification of an ecall; hogd-only |

Our enum does not distinguish load vs store; the oracle's mcause does. So the
trap comparison is coarser than the in-tree one: compare *"a fault was detected
at this pc, of a compatible class,"* recovering load/store from the trapping
opcode where we want the finer assertion. Never compare gas or our internal
enum value directly against the oracle.

## Axis 4 — ecall-boundary handling

A stock oracle has no notion of our host syscalls. On ecall:

- **(A) Stop at first ecall.** Compare only the pre-ecall trajectory. Simple,
  sound, limits coverage to ecall-free prefixes.
- **(B) Resync injection.** Let our `Machine` execute the host effect, then
  inject the same architectural result (set a0, advance pc) into the oracle so
  both continue in lockstep. Enables multi-ecall programs. Needs a steerable
  oracle (Spike can; Sail harder).
- **(C) ecall-free corpus.** Generate pure compute + memory programs only.
  Maximal *architectural* coverage with no boundary problem; no syscall
  coverage (already handled by the typed-syscall-layer tests and the in-tree
  crosscheck).

**Recommendation: (C) for the ISA-conformance pass** — it isolates exactly what
the external oracle is uniquely good at (independent ISA semantics), and the
ISA core is the thing we split `rv32_isa_core` out to validate. Add **(B)**
later if we want the external oracle to also cover the syscall *return* path.
The syscall *semantics* are not an ISA-conformance concern and belong with the
typed-layer tests.

## Corpus — where the programs come from

Three sources, in priority order:

1. **Constrained-random generator.** A fixed-seed generator emitting valid
   RV32IM in the supported profile, within 64 KiB, ecall-free (mode C),
   deliberately separating code and data so the Harvard split is a non-issue.
   This is where coverage actually comes from and is the main net-new build.
   (The spec-model crosscheck already has a fixed-seed generator we can learn
   from / extend.)
2. **Real ELF via the lowerer.** Our `ELF → v4 artifact` lowerer and Spike both
   consume ELF, so **ELF is a shared input format** — a clean equivalence point
   *iff* the lowerer's memory map (code/data placement, rodata relocation) is
   mirrored in the oracle config. This is the path to differential-testing real
   compiled contracts, and it interacts directly with impedance item #1.
3. **Upstream riscv-tests programs.** Ties into the open decision in
   `rv32-upstream-test-plan.md` Phase 1: *compile upstream assembly in CI* vs
   *check in generated artifact bytes*. The external oracle makes the compiled
   path more valuable (run the same compiled program both places).

## Phased plan

- **C1 — Scope + decision (this doc).** Pick the oracle and phase-1 mode; pin
  the equivalence checklist.
- **C2 — Oracle interface, in-tree adapter first.** Define an `Oracle`
  interface that yields an `ArchState` sequence for a given program + initial
  state, encoding the boundary contract (gas excluded, trap mapping, memory-map
  equivalence, stop-at-trap/ecall). Back it first with our existing hand spec
  model so the harness shape is proven with **zero external dependency**. (We
  effectively already did half of this migrating the crosscheck onto
  `ArchState`.)
- **C3 — Wire Spike behind an opt-in.** A CMake option / env guard so the core
  CI does **not** grow a Spike build dependency by default. Adapter parses the
  commit log (or links `processor_t`) → `ArchState` sequence; lockstep via
  `describe_trace_divergence`.
- **C4 — Constrained-random corpus generator.** The coverage engine; feeds both
  lockstep (small programs) and end-state fuzzing (volume).
- **C5 — (optional) sail-riscv high-assurance gate.** Second adapter behind the
  C2 interface; periodic, not per-CI.

## Decisions (locked for C2)

1. **Oracle: Spike first.** sail-riscv added later as a second adapter behind
   the C2 interface (C5).
2. **Phase-1 ecall mode: ecall-free corpus (C).** Resync injection (B) is a
   later phase if the external oracle should also cover the syscall return path.
3. **Dependency posture: opt-in / out of default CI.** Spike behind a CMake
   option + env guard; the core CI build stays dependency-free; the differential
   pass runs in a dedicated/periodic job. The consensus-CI critical path does
   not grow an external-simulator build/maintenance surface.
