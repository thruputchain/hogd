# RV32 Guest-Side Dispatch and the v4 Artifact Cut

Status: proposal (2026-06-01; revised after code-grounded review). This is the
execution contract for **Phase 3** of
[rv32-author-toolchain.md](rv32-author-toolchain.md): it proposes closure of that
note's open questions #1 ("how does the guest learn the invoked method id?") and
#2 ("artifact v4?") — final once Decision 2 is signed off — and settles the
`_start` ABI it left unsettled. The *decision*
to adopt guest-side dispatch is already recorded there; this note pins the
concrete entry ABI, the v4 layout, the version bumps, and the cross-repo
sequencing the three teams build against.

It does not re-argue the strategy. Read the author-toolchain "Dispatch: adopt
guest-side dispatch" section first for the why; this is the how.

**One decision gates everything below and is not yet made: Decision 2
(unknown-method semantics).** It determines whether v4 is protocol-visible and
even changes the exact v4 header layout. It needs Geoff/runtime-side sign-off
before this note becomes a decision record.

## What changes, in one paragraph

Today the runtime owns dispatch: the lowerer scans `.symtab` for the `16-HEX`
entrypoint symbols, writes a sorted `method_id → PC` table into the artifact, and
`runtime_context.cc` calls `entry_pc_for_method()` to pick the entry PC and
constructs `Machine(program, memory, gas, entry_pc)`. v4 moves the
**method → code** mapping into the guest: the runtime always enters at `_start`
(PC 0, already enforced), seeds the invoked `method_id` into registers, and the
SDK-generated `_start` runs its own `match method_id { … }`. The dispatch PC
table disappears from the artifact, the lowerer stops needing symbols (stripped
release builds work). What stays open is whether the runtime keeps a host-side
*existence* check (Decision 2).

The address model needs no change: the lowerer already requires `e_entry == 0`
(`crates/rv32/lower/src/elf.rs`) and `_start` at vaddr 0
(`crates/rv32/lower/src/lower.rs`), and the linker bases `.text` at 0 with
`ENTRY(_start)`. Only *what the runtime does at PC 0* changes.

## Decision 1 — entry ABI (`entry_abi_version` 1 → 2)

The runtime enters every call at PC 0 (`_start`) and passes the invoked method id
in registers:

- `method_id` is the existing **u64** selector (dispatch entries already carry an
  8-byte `method_id`; SDK selectors are `const SELECTOR: u64`).
- Split little-endian across two registers: **`a0 = method_id & 0xFFFF_FFFF`
  (low 32), `a1 = method_id >> 32` (high 32)**.
- The runtime seeds these **after `reset(gas)`** — which zeros the register file
  and sets `sp` (`machine.cc:62`) — and **after initial-memory staging**, and
  **before `run()`**. (The register-native syscall ABI only begins at the guest's
  first `ecall`, where `a0..a7` are snapshotted — `machine.cc:581` — so using
  `a0`/`a1` at process entry does not collide with it.)
- **Guest `_start` invariant (normative, must be proven in sdk_rs):** `_start` is
  a `global_asm!`/`#[naked]` shim that copies `a0`/`a1` into callee-saved
  registers (or the stack) as its *first* instructions, before any Rust prologue
  or runtime initialization runs. The current smoke fixture delegates `_start`
  entirely to `groundhog_rv32_runtime`
  (`built_contracts/integration_test_contracts/rv32_smoke_counter/src/main.rs:6`),
  so the capture lives in `rv32-runtime`'s `_start` and must be implemented and
  tested there — not assumed at the contract level, and not left to a plain
  `extern "C" fn _start` whose prologue may touch `a0`/`a1`.
- After the selected method wrapper returns, `_start` must terminate the machine
  deterministically (existing return/exit path), never fall through.

The register-passing choice (over a `get_invoked_method` syscall) is
author-toolchain OQ#1's "cleanest" option: no extra syscall, no host round-trip.

The syscall ABI (`syscall_abi_version`) is unchanged.

## Decision 2 — unknown-method semantics (the production decision; UNRESOLVED)

This is the real fork, and my first draft got it wrong by calling for the "same
host-visible outcome via a return/revert path" — those are contradictory, because
the current outcome is *pre-execution*.

**Current behavior (RV32 and WASM, identical):** an unknown method is a
deterministic miss *before any guest code runs* — `InvokeErrorCode::Deterministic`
with **0 gas**, no guest function run, no syscalls, no trace (RV32 builds no
`Machine` at all; WASM has an instance but invokes nothing):

- RV32: `entry_pc_for_method()` returns `nullopt` → `{InvokeError{Deterministic}, 0}`
  in `runtime_context.cc` (no `Machine` is built); test
  `UnknownDispatchMethodIsDeterministicMiss` (`rv32_runtime_context_test.cc`,
  `kUnknownMethodId = 2`).
- WASM: on an already-created instance, `instance.get_func(method)` → `None` →
  `InvokeErrorCode::Deterministic` (`hogd/rust/wasmtime/src/runtime.rs:121`,
  `hogd/rust/wasmi/src/wasmi_runtime.rs:142`). The parity point is **no guest
  function runs, zero gas, no syscalls** — not "no instance," which the runtime
  has already built.

If v4 dispatches unknown methods *in the guest*, the observable contract changes:
a `Machine` is constructed, the dispatcher's compare chain retires instructions
(metered gas, nonzero), and the trace is non-empty. That is a **protocol-visible
divergence from WASM**. So this must be a deliberate choice, not a side effect.

**Option A — metered guest miss.** The guest dispatcher's default arm produces a
deterministic error/abort. Simplest artifact and runtime; no host existence
check. Cost: unknown methods now consume gas and emit a guest trace, and RV32
diverges from WASM's zero-gas function-not-found. Acceptable only if we decide
unknown-method gas/trace parity across runtimes is not a protocol requirement.

**Option B — preserve the zero-gas host miss (recommended).** Keep the
pre-execution deterministic miss by checking the invoked id against the
contract's **method set, sourced outside the artifact PC table**; only enter the
guest for known ids. RV32 stays byte-for-byte WASM-compatible on the
unknown-method path. The guest's default arm becomes an unreachable backstop
(host already filtered) that deterministically traps if ever reached. This is the
reviewer's lean and preserves error/gas/trace parity; it costs a host-side
existence check.

Under Option B, **where the method set lives** is a sub-decision (both keep the
win of dropping the PC table — no PCs, no symbol dependence, strip is fine):

- **B-ii — minimal method-id set in the artifact.** A sorted array of 8-byte
  `method_id`s (existence-only: **no PCs, no per-entry PC/instruction-boundary
  validation**). Half the per-entry size of the v3 dispatch table, host-checkable
  in O(log n), and decoupled from code layout/symbols. Changes the v4 header (see
  Decision 3, variant B-ii).
- **B-iii — out-of-band manifest.** The runtime gets the method set from the
  deploy manifest rather than the artifact. Cleanest artifact, but moves the
  contract to the deploy path.

(An earlier draft listed a "B-i: hogd parses the embedded contract spec" option.
It is **not implementable**: the artifact body is `header || code ||
initial_memory` with no section directory, and the lowerer deliberately *excludes*
the `groundhogspecv0`/`groundhogspecrawv0` sections from the body — they are
compile-time sidecars, not VM memory (`crates/rv32/lower/src/lower.rs:20`). The
artifact carries no spec to parse. Dropped.)

**Recommendation:** Option B for production parity. Between sources, **B-ii** is
the cheapest runtime contract that keeps the strip/fragility wins (no deploy-path
coupling, a tiny host-checkable section); B-iii if we prefer the artifact carry
nothing and the method set ride the deploy manifest. **This is the decision to
make before freezing the v4 header.**

## Decision 3 — artifact v4 layout (`format_version` 3 → 4)

`entry_abi_version` bumps to **2** (calling convention) and `format_version` to
**4** (layout) — independent header fields, both change. The format fingerprint
domain changes `"...format.v3"` → `"...format.v4"` and the canonical section
order drops `dispatch`; re-pin `FORMAT_FINGERPRINT` (Rust
`groundhog-rv32-artifact`) and `kArtifactFormatFingerprint` (hogd `artifact.h`)
per [rv32-artifact-format-drift.md](rv32-artifact-format-drift.md). The
cross-check makes the cut atomic: a v4 artifact is rejected by a v3 hogd and vice
versa.

The artifact crate + `artifact.h` remain the enforced source of truth; the tables
below are the design intent the implementation must realize, and the fingerprint
is the enforcement.

**Variant for Options A / B-iii — dispatch section fully removed (`header_bytes`
172 → 132).** The "off" column is each field's *header position*; the "vs v3"
column flags whether that position **and/or the stored value** changes.
"unchanged" means both; value-only changes are called out explicitly.

```text
  off  size  field                      vs v3
    0     8  magic "GHRV32A1"           unchanged
    8     4  format_version      (4)    value 3 → 4
   12     4  header_bytes        (132)  value 172 → 132
   16     8  vm_profile_digest          unchanged
   24     4  flags               (0)    unchanged
   28     4  entry_abi_version   (2)    value 1 → 2
   32     4  syscall_abi_version (1)    unchanged
   36     4  gas_table_version   (2)    unchanged
   40     8  gas_schedule_digest        unchanged
   48     4  memory_bytes               unchanged pos; per-contract (see Decision 5)
   52     4  code_size                  unchanged
   56     4  initial_memory_size        unchanged pos; ≤ memory_bytes (see Decision 5)
   60     4  code_offset                pos unchanged; VALUE 172 → 132 (header shrank)
   64     4  initial_memory_offset      pos unchanged; VALUE shifts (no dispatch gap)
   68    32  sha256(code)               position was 76
  100    32  sha256(initial_memory)     position was 108
```

Body: `header || code || initial_memory`. Removed:
`dispatch_offset`(was 68), `dispatch_size`(was 72), `sha256(dispatch_table)`(was 140).
`memory_bytes` is **not** a fixed `65536` — it is per-contract, nonzero, ≤
`MAX_MEMORY_BYTES` (today `artifact.cc:150`), and Decision 5 governs its relation
to `initial_memory_size`.

**Variant B-ii — method-id existence set retained (`header_bytes` stays 172):**
replace the two dispatch *offset* fields and the dispatch digest with a method-set
section of the same header footprint but id-only body:

```text
   68     4  method_set_offset
   72     4  method_set_size      (= n * 8)
   76    32  sha256(code)               position unchanged from v3
  108    32  sha256(initial_memory)     position unchanged from v3
  140    32  sha256(method_set)
```

Body: `header || code || method_set || initial_memory`, where `method_set` is
`n` sorted little-endian `u64` ids. `header_bytes` stays 172, but `code_offset`'s
value is still 172 only if no Decision-5 shrink applies to *code* (it doesn't —
code is always materialized), and `initial_memory_offset`'s value shifts by the
`method_set` size relative to v3. The same per-contract `memory_bytes` /
`initial_memory_size ≤ memory_bytes` rules (Decision 5) apply.

**Removed validation invariants (all variants):** the dispatch table's
`dispatch_size % DISPATCH_ENTRY_BYTES == 0`, sorted+unique method ids *with PCs*,
each PC in-code + 4-aligned + on an instruction boundary, `MAX_DISPATCH_ENTRIES`
bound (`artifact.cc:164`), the dispatch-table digest match, and the dispatch
limits folded into the fingerprint (`artifact.h:244`).

**Variant B-ii must replace those bounds, not just drop them.** The method-set
section needs its own caps, all folded into the v4 fingerprint: `method_set_size`
a multiple of 8, `method_set_size ≤ kMaxMethodSetBytes`, an entry-count cap
(`n ≤ kMaxMethods`), method ids sorted + unique, and the `sha256(method_set)`
match. Decide whether a **zero-method** contract is valid (`n == 0`); if not,
require `method_set_size > 0`. (PC validation is the only thing genuinely gone —
the *bounds* discipline carries over in id-only form.)

## Decision 4 — lowerer + SDK simplification

**Lowerer (`groundhog-rv32-lower`):**

- Remove `discover_dispatch_entries` and the dispatch-table build/commit.
- Stop requiring method symbols in `.symtab`. Validate the entry from the ELF
  header (`e_entry == 0`) plus PT_LOAD layout — no symbol table needed. The
  current `_start`-symbol lookup becomes redundant with `e_entry == 0`; delete it.
- Drop the "missing entrypoint symbols" errors/hints and the
  `CARGO_PROFILE_RELEASE_STRIP=false` requirement. **Normal stripped release
  builds are valid v4 inputs.**
- Metadata stops reporting `dispatch_entries`. (Confirm no downstream reader — CLI,
  `client-ts` regen, sidecar consumers — depends on it; it was rv32-lower-specific.)
- Under Option B-ii only: the lowerer writes the sorted method-id set. The ids come
  from the contract spec/macro, **not** from scanned symbols, so strip still works.

**SDK (`#[contract]` macro + `rv32-runtime`):**

- `_start` becomes the dispatcher (Decision 1's naked shim → capture `a0`/`a1` →
  `match method_id` → the same internal method wrappers used today → default arm
  per Decision 2).
- The per-method `#[export_name="<16-HEX>"]` / `.text.gh_*` entry symbols are no
  longer the dispatch surface; the linker no longer needs `KEEP(*(.text.gh_*))`
  for dispatch.
- **Spec fragments are unchanged.** They emit into the `groundhogspecv0` /
  `groundhogspecrawv0` ALLOC sections (retained via `#[used]` gated to
  `target_arch = "riscv32"`); stripping removes `.symtab`/debug, not ALLOC
  sections, so `.spec.json`/`.spec.xdr` emission survives stripped builds — prove
  this with a test rather than assume it.

## Decision 5 — zero-extended initial memory (artifact size)

Today the artifact carries a **fully materialized** data-memory image:
`artifact.cc:161` rejects `initial_memory_size != memory_bytes`, so a contract
that declares 64 KiB of guest data memory ships 64 KiB in the artifact even
though most of it is zero (bss/heap/stack). That is pure bloat.

**Two distinct quantities — do not conflate them (this is the trap):**

- `memory_bytes` is the **total guest data address space** — it sets `sp` to the
  top of memory (`machine.cc:62`, `threaded_machine.h:131`), so it is the SDK
  linker's RAM length / ABI memory size, **not** a PT_LOAD `memsz`. Decision 5
  does **not** shrink it. Deriving it from loaded-segment size would move the
  stack down to end-of-data and corrupt execution (the fixture linker declares
  64 KiB RAM without reserving stack in a load segment —
  `fixtures/compiled_sha256_return/linker.ld:5`).
- `initial_memory_size` is the **stored prefix** of that address space — this is
  what Decision 5 shrinks.

The data image is **address-indexed**, per the modified-Harvard model
(`rv32-vm-experimental-design.md:158`): `.text` lives in separate code memory and
its data-memory shadow is inert. The official `rv32-lower` follows this shape for
the fixture: start a zero-filled image of size `memory_bytes`, validate the ALLOC
section policy, and copy only **VM-memory PROGBITS sections from the lowerer
allowlist** at their guest virtual addresses (for example
`.rodata`/`.srodata`/`.data`/`.sdata`).
Skip NOBITS sections for contents (`.bss`/`.sbss` are provided by the zero-fill),
and explicitly exclude both `.text` and metadata/spec sidecars
(`groundhogspecv0`/`groundhogspecrawv0`) from the VM-memory image. **Do not**
package raw ELF `filesz` or concatenate `.rodata`/`.data` — that misplaces
addresses and can feed code or metadata bytes in as data.

The change is only to stop padding the trailing zeros:

- Relax the rule to `initial_memory_size ≤ memory_bytes` (drop the `!=` check at
  `artifact.cc:161`; keep the `≤` check at `:156`).
- The lowerer stores the prefix `[0, high_water)`, where `high_water` is the end
  address of the highest-addressed copied section, instead of padding to
  `memory_bytes`. (The prefix still contains address-preserving zeros below the
  first initialized section — the `.text` shadow and any gaps — since storage
  starts at offset 0; the win is eliding the large zero **tail**: bss, heap, and
  the stack region above `high_water`.)
- The runtime needs **no new logic**: `machine.cc:38,46` already
  `resize(memory_bytes)` then `fill(memory_, 0)` before staging, so
  `[initial_memory_size, memory_bytes)` is already zero. It copies the
  `initial_memory_size`-byte prefix at offset 0; the rest stays zero.
- The `sha256(initial_memory)` commitment is over the **stored prefix bytes**,
  which is what's stored — unchanged digest discipline.

No header-layout change (`memory_bytes` and `initial_memory_size` are already
separate fields); this is a validation-rule + body-content change only, and it
directly answers review finding 2 (don't force full 64 KiB images). A typical
counter then ships code + its data high-water prefix instead of 64 KiB.

It is technically separable from dispatch, but the v4 format/fingerprint/fixtures
are already churning, so fold it into the same cut. Determinism is unaffected
(zero-fill is deterministic). Add an artifact-size assertion to the smoke
(below).

## Consume-side change (hogd) — blast radius

Conceptually small, but it removes dispatch *state*, not just one call site. Do
not leave a zombie dispatch model alive in tests:

- `runtime_context.cc`: stop calling `entry_pc_for_method()`; construct
  `Machine(program, memory_bytes, gas_limit)` (i.e. `initial_pc = 0`, already the
  constructor default — today the call site passes `*entry_pc`); seed `a0`/`a1`
  after `reset()`+memory staging, before `run()`; remove the host miss path (under
  Option A) or replace it with the method-set existence check (Option B).
- `runtime_context.h`: remove `dispatch_table` from `RuntimeContractImage` and the
  default-to-method-0 behavior (`runtime_context.h:20`).
- `artifact.{h,cc}`: stop parsing/returning dispatch state (`artifact.h:319`);
  drop the dispatch fields from the fingerprint fold (`artifact.h:220`) and the
  dispatch validation (`artifact.cc:115`). Drop the
  `initial_memory_size != memory_bytes` rejection (`artifact.cc:161`) per
  Decision 5, keeping the `≤` check. Under B-ii, add the method-set
  parse/validate instead.
- `runtime_context.cc:63`: artifact validity no longer depends on
  `dispatch_table_is_valid`.
- **Both interpreters** need the entry ABI: `Machine` and the direct-threaded
  `ThreadedMachine` (`threaded_machine.h`, `initial_pc = 0` constructor). If the
  threaded interpreter remains a production candidate, the seam and seeding apply
  to it too.
- Tests: `rv32_runtime_context_test.cc` (incl. `UnknownDispatchMethodIsDeterministicMiss`,
  whose *meaning* changes under Option A vs B) and the artifact tests.

**Machine seam.** Add a dedicated `Machine::seed_method_id(uint64_t)` (or a
`reset`/constructor parameter) — **not** a general public `set_reg`. `machine.h`
today exposes a public read `reg(size_t)` and a private `write_reg(uint8_t,
uint32_t)` over `std::array<uint32_t, kRegisterCount> registers_`; the seam is a
narrow public addition. `MachineTestPeer` (already a friend) can write registers
today, so the feasibility spike needs no production API to run.

## Determinism and gas notes

- **Determinism:** seeding `a0`/`a1` and the guest `match` are deterministic;
  consensus is unaffected. The unknown-method arm must be deterministic too
  (both options satisfy this).
- **Gas:** the **method → code** dispatch is now in-guest and metered; cost is
  data-dependent (which arm), and a linear `match` is O(methods). Under Option A
  the unknown path is also metered (the parity change above); under Option B it
  stays zero-gas. Don't optimize preemptively, but know the dispatcher codegen
  shape (linear vs sorted/jump-table) affects per-method gas fairness as the
  method count grows.

## Open question / feasibility spike (gates the ABI)

The seam *shape* is decided (`seed_method_id`, both interpreters). The remaining
spike: **do the seeded `a0`/`a1` arrive intact and survive `_start`'s prologue?**
Prototype with `MachineTestPeer`: reset, seed `a0`/`a1`, `run()` a tiny hand-built
program that reads them and `ecall`s a return; assert the values arrive. Repeat
for `ThreadedMachine`.

## Drift to clean up while cutting v4

These are stale because of the strip/dispatch change and should land with the cut:

- `built_contracts/build_risc.sh:42` forces `CARGO_PROFILE_RELEASE_STRIP=false` —
  no longer needed (v4 accepts stripped builds).
- `built_contracts/CMakeLists.txt:290` comment excludes raw contracts for lacking
  `gh_*` symbols — update; `gh_*` is no longer the dispatch surface.

## Sequencing

The fingerprint cross-check makes this an atomic cut: don't ship dual v3/v4
support in hogd (no users to protect). Land hogd-v4, lowerer-v4, and the SDK
dispatcher together, regen all fixtures in one set, and run the smoke against
matching versions.

0. **Decide Decision 2.** It gates the v4 header and whether v4 is
   protocol-visible. Everything downstream depends on it.
1. **Spike (hogd):** register-seed + run-from-0 via `MachineTestPeer`, on both
   interpreters. → confirms the ABI is implementable.
2. **Lock the ABI + format:** this doc to `Status: decision record`; pin the
   chosen v4 header table and re-pin the fingerprint across both sides.
3. **Parallel (independent once ABI + format are agreed):**
   - *sdk_rs:* naked `_start` dispatcher + unknown-method arm; drop the
     `.text.gh_*` dispatch dependence; unit-test the generated `match` and the
     `a0`/`a1` capture in isolation.
   - *groundhog-rpc:* artifact crate v4 (chosen variant, re-pin fingerprint,
     update static asserts/tests); thin the lowerer to validate-via-`e_entry` +
     package + hash; accept stripped builds.
   - *hogd:* `runtime_context.{h,cc}` + `artifact.{h,cc}` (remove dispatch state /
     add method-set check); `seed_method_id` on both interpreters; re-pinned
     `kArtifactFormatFingerprint`; update tests; drift cleanup.
4. **Real-calldata smoke (hogd, closes author-toolchain Phase 0/4):** build with
   `groundhog build --target rv32`, generate the trait header from `.spec.json`,
   and from a native driver call `increment(7)` then assert `get() == 7`. Add:
   - an **unknown method-id** call asserting the decided outcome (Option A: metered
     deterministic error; Option B: zero-gas `Deterministic` miss matching WASM),
   - a **stripped release build** that still emits valid `.spec.json`/`.spec.xdr`,
   - a method-with-args vs no-arg pair, so calldata read is shown independent of
     dispatch,
   - an **artifact-size assertion** (Decision 5): `initial_memory_size <
     memory_bytes` and the counter's `.rv32a` is smaller than the old full-image
     size. (Not strictly `code + materialized data`: the stored prefix still
     carries address-preserving zeros below the first initialized section.)

## Cross-references

- [rv32-author-toolchain.md](rv32-author-toolchain.md) — the dispatch decision and
  3-repo ownership split. This note executes its Phase 3 and proposes closure of
  open questions #1 and #2. When Decision 2 is signed off and v4 lands, mark
  those as closed-by-this.
- [rv32-artifact-format-drift.md](rv32-artifact-format-drift.md) — the fingerprint
  cross-check that enforces the v3 → v4 cut on both sides.
- [rv32-vm-experimental-design.md](rv32-vm-experimental-design.md) — VM/runtime
  design this assumes.
