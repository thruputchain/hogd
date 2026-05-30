# RV32 Author Toolchain: Lowerer, Build, and Dispatch

Status: proposal / decision record. Drafted 2026-05-29 off the `counter-rv32`
spike and the landing of the register-native syscall ABI (groundhog-xdr 0.14.x,
hogd PRs #2411 / #2417 and the `rv32: switch syscall bridge to register-native`
work).

This note answers the **"Production SDK lowerer design"** gate that
[rv32-cpp-ownership-review.md](rv32-cpp-ownership-review.md) lists as still open,
and it records where author-side tooling should live across repos. It assumes
the VM/runtime design in [rv32-vm-experimental-design.md](rv32-vm-experimental-design.md).

## Context

The spike proved minimal viability end to end:

- `counter-rv32` (a real SDK contract, not the `compiled_sha256_return` fixture)
  builds for `riscv32im-unknown-none-elf`.
- The `#[contract]` macro emits each entrypoint as `#[export_name = "<16-HEX>"]`
  + `#[link_section = ".text.gh_<16-HEX>"]` on rv32.
- The existing `scripts/rv32_lower.py` lowers that ELF to a valid v3 artifact
  with all four dispatch entries resolved to PCs.
- hogd's `LoweredSdkArtifactSmoke` test
  (`rv32_runtime_context_test.cc`) parses such a lowered artifact and invokes its
  lowest-ID dispatch entry through `root_invoke()` — so the VM-side path exists.
  It's env-var-plumbed (`RV32_SMOKE_ARTIFACT`) and informational (`EXPECT`, not
  `ASSERT`); real calldata and a CI-built (non-env-var) artifact are still open.

It also surfaced two staleness/ergonomics problems that motivate this design:

1. The release build strips `.symtab` by default, so the symbol-scanning lowerer
   finds zero entrypoints unless the contract is built `strip = false`.
2. `rv32_lower.py` is a second, drifting implementation of the artifact format.
   It was stamping `syscall_abi = 0` against a VM that now validates `= 1` — the
   exact failure mode you get from maintaining two copies of one contract.

## Goals and constraints

- **North star:** `groundhog build` produces a deployable rv32 artifact.
- **The CLI is a convenience layer, not a gate.** You can already get a WASM
  artifact without the `groundhog` CLI; the CLI exists to hide "a ton of annoying
  configs." The rv32 lowerer must be usable the same way: a standalone,
  scriptable binary you can drop in a Makefile/CI step — the `wasm-opt` model.
  (`groundhog build` already locates wasm-opt with `which::which` and shells out;
  it does not embed it.)
- **One implementation.** A single Rust lowerer used by both the build path and
  hogd's own tests, replacing the python script — so the format can't drift.
- **Minimize cross-repo coupling.** The artifact format is the one hard contract;
  isolate it.

## Components and ownership

| Component | Repo | Role |
|---|---|---|
| `groundhog-rv32-artifact` (lib) | protocol-rs (next to `groundhog-xdr`) | **Single Rust source of truth** for the artifact format: header constants, writer, validator. |
| `groundhog-rv32-lower` (lib + bin `rv32-lower`) | groundhog-rpc (alongside CLI build tooling) | The `wasm-opt` analog: `rv32-lower --elf in --out x.rv32a`. Validate ELF layout → package code+memory → hash → write artifact. Depends on `groundhog-rv32-artifact`. (Crate keeps the `groundhog-` prefix; the binary is the short `rv32-lower`, as `groundhog-cli` → `groundhog`.) |
| `#[contract]` macro + `rv32-runtime` | sdk_rs | Entry symbols, the guest-side dispatcher (see below), and typed syscall wrappers. `_start` routes to the selected method once the method-id-passing mechanism is chosen (open question below) — the `_start` ABI is not yet settled. |
| `groundhog build` (rv32 path) | groundhog-rpc | Convenience orchestration: cargo-build the rv32 bin → shell out to `rv32-lower` → emit `.rv32a` + the contract spec it already produces. |
| VM / artifact loader / dispatch | hogd | Consume side. `artifact.h` pins the format constants in C++ today; making the Rust crate authoritative with a build-time digest cross-check is a Phase 1/2 acceptance criterion, not current state (Geoff). |

Why `groundhog-rv32-lower` lives in groundhog-rpc, not sdk_rs: the existing WASM
path already keeps build/instrument tooling in the CLI repo (sdk_rs has no wasm
lowerer — the CLI instruments wasm). The rv32 lowerer is tooling, so it mirrors
that split. It could alternatively be its own repo; what matters is that it is a
focused standalone binary shelled out to, not a library the big CLI must embed.

## Dispatch: adopt guest-side dispatch (Geoff)

The current mechanism (M4) is **runtime-side artifact dispatch**: the lowerer
scans `.symtab` for the `16-HEX` entrypoint symbols, builds a method-id → PC
table into the artifact, and the runtime validates it and selects the entry PC.

The proposal is **guest-side dispatch**: the contract carries its own
`match method_id { … }` switch behind a single entry; the runtime enters at one
PC and tells the guest which method was invoked.

| | Runtime-side table (today) | Guest-side switch (proposed) |
|---|---|---|
| Lowerer | Scans symbols, builds + commits a dispatch table | Validate + package + hash only |
| Symbols | Must retain `.symtab` (the strip issue) | Not needed — strip is fine |
| Fragility | method→PC depends on linker/codegen layout | None — one fixed entry |
| Method set visibility | In the artifact | In the contract spec (already emitted by `groundhog build`) |
| Unknown method | Runtime zero-gas deterministic miss | Guest dispatcher must be deterministic (abort/return-error, never panic) — small gas cost |
| Cross-repo coupling | Dispatch-table format shared 3 ways | Gone |

**Recommendation: adopt guest-side dispatch.** It deletes the single biggest
source of lowerer complexity, makes the standalone tool `wasm-opt`-sized, and
removes the dispatch-table cross-repo contract. The "method set must be readable
from the artifact" argument for the table evaporates because `groundhog build`
already emits the spec/manifest; indexers read the spec, not the artifact.

Divergence from WASM (named-export dispatch) is acceptable: parity tests compare
host-visible *outcomes*, not the dispatch mechanism.

The one non-negotiable: the macro-generated dispatcher must be provably
deterministic on unknown ids (abort or return an error code, never panic/trap),
since it now owns the unknown-method path the runtime used to guarantee.

## Artifact format: the load-bearing contract

A standalone lowerer cannot borrow format knowledge from the big CLI, so the
format must live in a crate both the tool and (via cross-check) hogd agree on.
The target end state: `groundhog-rv32-artifact` holds it as the single Rust
source of truth, with hogd's `artifact.h` cross-checking the VM-profile and
gas-schedule digests at build time.

Today neither side of that exists yet: `artifact.h` pins the constants in C++
with internal `static_assert`s, and `rv32_lower.py` only *documents* that such a
cross-check "should" exist. So this is a **Phase 1/2 acceptance criterion**, not
current state — landing `groundhog-rv32-artifact` must include the cross-check
(or an equivalent shared-constant mechanism) against `artifact.h`.

Current v3 surface to mirror (from `hogd/hogexec/runtime/rv32/artifact.h`):
magic `GHRV32A1`, 172-byte header, format version 3, entry ABI 1, syscall ABI 1,
gas table version 1 + gas-schedule digest `0xEC026096ED9849F7`, VM-profile digest
`0x55C53A685F0EB33D`, and SHA256 commitments for code / initial-memory / dispatch
sections.

If guest-side dispatch lands, the dispatch section + its commitment become
vestigial; either keep a trivial single-entry table under v3 for now or have hogd
cut a **v4** that drops the section. Keep the gas/profile digest discipline intact
through any bump.

## Build flow (the two paths)

Standalone (scriptable, no CLI config required):

```
cargo build --release --target riscv32im-unknown-none-elf -p <contract>-rv32
rv32-lower --elf target/.../release/<contract>-rv32 --out <contract>.rv32a
```

Convenience:

```
groundhog build            # cargo-build rv32 → shell out to rv32-lower → .rv32a + spec
```

`groundhog build` should locate `rv32-lower` the way it locates wasm-opt
(`which::which`) and emit a clean "install the lowerer" error if missing
(mirror `CliError::WasmOptNotFound`). Distribution: ship the `rv32-lower` binary
through whatever channel installs the `groundhog` CLI (e.g. `cargo install`), so
it lands on `$PATH` the way wasm-opt does.

## Open questions (Geoff-owned, runtime side)

1. **How does the guest learn the invoked method id?** Cleanest: the runtime
   drops `method_id` into a register at the entry PC and the SDK-generated
   `_start` reads it. Alternative: a `get_invoked_method` syscall (more moving
   parts). Needed before guest-side dispatch can land.
2. **Artifact v4?** Whether method_id-passing ships together with dropping the
   dispatch section, or v3 keeps a trivial table in the interim.
3. **Typed syscall wrappers + the known ABI gap.** The register-native ABI can't
   express a full 64-bit scalar by value (`MintAuthToken`'s `BlockHeight` expiry
   `>= 2^31`); resolve by passing such values by pointer. The SDK's typed syscall
   wrappers are where that convention gets enforced for authors.

## Sequencing

- **Phase 0 — done in spike:** rv32 build, entry symbols, python lowering to a
  valid v3 artifact (header now correct after the local `syscall_abi 0→1` fix),
  and execution through the `LoweredSdkArtifactSmoke` harness (parse + invoke
  first dispatch entry). Still open: running it with real calldata and plumbing a
  CI-built artifact instead of the `RV32_SMOKE_ARTIFACT` env var.
- **Phase 1:** stand up `groundhog-rv32-artifact` (format truth) and port the
  python lowerer to the `groundhog-rv32-lower` crate (lib + bin `rv32-lower`) at
  output parity with the python script.
- **Phase 2:** point hogd's `rv32_runtime_context_test` smoke test at the
  `rv32-lower` binary; delete `scripts/rv32_lower.py`. One implementation from
  here on.
- **Phase 3 (needs Geoff):** method_id-passing + guest-side dispatcher in the
  SDK macro; thin the lowerer to validate+package+hash; optionally cut artifact
  v4. The strip-symbols requirement disappears here.
- **Phase 4:** wire the `groundhog build` rv32 path as the convenience layer over
  the standalone tool.

Phases 1–2 are valuable on their own (they kill the two-implementations drift)
and do not block on the runtime-side dispatch decision in Phase 3.
