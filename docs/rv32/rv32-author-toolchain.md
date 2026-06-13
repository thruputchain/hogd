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
- The original `scripts/rv32_lower.py` lowered that ELF to a valid v3 artifact
  with all four dispatch entries resolved to PCs.
- hogd's `LoweredSdkArtifactSmoke` test
  (`rv32_runtime_context_test.cc`) parses such a lowered artifact and invokes its
  lowest-ID dispatch entry through `root_invoke()` — so the VM-side path exists.
  It's env-var-plumbed (`RV32_SMOKE_ARTIFACT`) and informational (`EXPECT`, not
  `ASSERT`); real calldata and a CI-built (non-env-var) artifact are still open.

It also surfaced two staleness/ergonomics problems that motivate this design:

1. The release build strips `.symtab` by default, so the symbol-scanning lowerer
   finds zero entrypoints unless the contract is built `strip = false`.
2. `rv32_lower.py` was a second, drifting implementation of the artifact format.
   It stamped `syscall_abi = 0` against a VM that validated `= 1` — the exact
   failure mode you get from maintaining two copies of one contract.

## Goals and constraints

- **North star:** `groundhog build` produces a deployable rv32 artifact.
- **The CLI is a convenience layer, not a gate.** You can already get a WASM
  artifact without the `groundhog` CLI; the CLI exists to hide "a ton of annoying
  configs." The rv32 lowerer must remain usable the same way: a standalone,
  scriptable binary you can drop in a Makefile/CI step. For the CLI path,
  `groundhog build --target rv32` can call the same lowerer as a Rust library to
  avoid an extra binary install and shell-out.
- **One implementation.** A single Rust lowerer used by both the build path and
  hogd's own tests, replacing the python script — so the format can't drift.
- **Minimize cross-repo coupling.** The artifact format is the one hard contract;
  isolate it.

## Components and ownership

| Component | Repo | Role |
|---|---|---|
| `groundhog-rv32-artifact` (lib) | groundhog-rpc (`crates/rv32/artifact`) | **Single Rust source of truth** for the artifact format: header constants, writer, validator. |
| `groundhog-rv32-lower` (lib + bin `rv32-lower`) | groundhog-rpc (alongside CLI build tooling) | The `wasm-opt` analog: `rv32-lower --elf in --out x.rv32a`. Validate ELF layout → package code+memory → hash → write artifact. Depends on `groundhog-rv32-artifact`. (Crate keeps the `groundhog-` prefix; the binary is the short `rv32-lower`, as `groundhog-cli` → `groundhog`.) |
| `#[contract]` macro + `rv32-runtime` | sdk_rs | Entry symbols, the guest-side dispatcher (see below), and typed syscall wrappers. `_start` routes to the selected method once the method-id-passing mechanism is chosen (open question below) — the `_start` ABI is not yet settled. |
| `groundhog build` (rv32 path) | groundhog-rpc | Convenience orchestration: cargo-build the rv32 bin → call the lowerer library → emit `.rv32a` + the contract spec/metadata it already produces. |
| VM / artifact loader / dispatch | hogd | Consume side. `artifact.h` pins the format constants in C++ and exposes the matching format fingerprint; hogd's smoke metadata check proves the consumed lowerer/runtime pair agree. |

Why `groundhog-rv32-lower` lives in groundhog-rpc, not sdk_rs: the existing WASM
path already keeps build/instrument tooling in the CLI repo (sdk_rs has no wasm
lowerer — the CLI instruments wasm). The rv32 lowerer is tooling, so it mirrors
that split. It could alternatively be its own repo; what matters is that the
format/lowering implementation is focused and reusable: a library for
`groundhog build`, plus a standalone `rv32-lower` binary for scripts and direct
interop checks.

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
| Unknown method | Runtime zero-gas deterministic miss | Must be deterministic; whether it stays a zero-gas host miss or becomes a metered guest outcome is the open production decision — see [rv32-guest-dispatch-v4.md](rv32-guest-dispatch-v4.md) Decision 2 |
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

As of 2026-05-31 the Rust format implementation has been promoted into the
standalone `groundhog-rv32-artifact` crate. Rust and C++ each pin the same
`FORMAT_FINGERPRINT`/`kArtifactFormatFingerprint`, and hogd's generated-smoke
metadata check verifies the stamped lowerer fingerprint against `artifact.h`.
That gives the format extraction a regression guard and keeps cross-repo drift
visible while the implementation remains duplicated across languages.

Current v3 surface to mirror (from `hogd/hogexec/runtime/rv32/artifact.h`):
magic `GHRV32A1`, 172-byte header, format version 3, entry ABI 1, syscall ABI 1,
gas table version 2 + gas-schedule digest `0xEBE53DCEEA581D0C`, VM-profile digest
`0x55C53A685F0EB33D`, and SHA256 commitments for code / initial-memory / dispatch
sections.

If guest-side dispatch lands, the dispatch section + its commitment become
vestigial; either keep a trivial single-entry table under v3 for now or have hogd
cut a **v4** that drops the section. Keep the gas/profile digest discipline intact
through any bump.

## Build flow (the two paths)

Standalone (scriptable, no CLI config required):

```
CARGO_PROFILE_RELEASE_STRIP=false \
  cargo build --release --target riscv32im-unknown-none-elf -p <contract>-rv32
rv32-lower --elf target/.../release/<contract>-rv32 --out <contract>.rv32a
```

Hogd's `build_risc.sh` helper sits alongside `build_wasm.sh`: it builds the ITC
RV32 contract with Cargo, lowers it with the pinned published `rv32-lower`
binary, and writes the artifact plus metadata sidecar into the requested build
directory:

```
built_contracts/build_risc.sh rv32_smoke_counter /tmp/rv32-smoke
RV32_SMOKE_ARTIFACT=/tmp/rv32-smoke/rv32_smoke_counter.rv32a \
RV32_SMOKE_METHOD_ID=0x143934D51BD51575 \
  build/gcc-rv32-ci/hogd/hogexec/runtime/rv32_runtime_context_test \
  --gtest_filter='*LoweredSdkArtifactSmoke'
```

`CARGO_PROFILE_RELEASE_STRIP=false` is required while v3 lowering discovers
`_start` and dispatch PCs from the ELF symbol table. The method id above is the
smoke counter's no-arg `get` entrypoint; calldata-taking methods are still useful
failure-path smoke checks, but they abort on empty calldata.

Convenience:

```
groundhog build --target rv32  # cargo-build rv32 → lower in-process → .rv32a + metadata/spec
```

`groundhog build --target rv32` should be the golden author/CI path once the
library-backed lowerer path has settled: it builds the RV32 ELF and invokes the
lowerer in-process, so users do not need a separate `rv32-lower` binary just to
build a contract. Keep shipping `rv32-lower` for Makefiles, direct scripting,
manual interop checks, and hogd-side pinned smoke tests while they still need an
explicit tool boundary.

**Follow-up:** hogd currently exercises WASM contracts through
`built_contracts/build_wasm.sh`, which wraps `groundhog build --json`, but its
RV32 smoke artifact still goes through `built_contracts/build_risc.sh` →
`scripts/rv32_make_smoke_artifact.sh` → pinned `rv32-lower`. Once the
`groundhog build --target rv32` output is accepted as the golden path, migrate
hogd's RV32 CI/integration smoke to call `groundhog build --target rv32 --json`
too. At that point the WASM and RV32 contract fixtures share one clean build
entrypoint, and `rv32-lower` remains the lower-level scriptable escape hatch.

## Open questions (Geoff-owned, runtime side)

Questions 1 and 2 below have a proposed resolution in
[rv32-guest-dispatch-v4.md](rv32-guest-dispatch-v4.md) (the Phase 3 execution
contract): method_id passed in `a0`/`a1`, shipped together with a v4 cut that
drops the dispatch section. That note also settles the `_start` ABI left
unsettled in the components table above.

1. **How does the guest learn the invoked method id?** Cleanest: the runtime
   drops `method_id` into a register at the entry PC and the SDK-generated
   `_start` reads it. Alternative: a `get_invoked_method` syscall (more moving
   parts). Needed before guest-side dispatch can land.
2. **Artifact v4?** Whether method_id-passing ships together with dropping the
   dispatch section, or v3 keeps a trivial table in the interim.
3. **Typed syscall wrappers.** The register-native ABI cannot express a full
   64-bit scalar by value (`MintAuthToken`'s `BlockHeight`, for example), so
   wrappers should pass such values by pointer. The SDK's typed syscall wrappers
   are where that convention gets enforced for authors.

## Sequencing

- **Phase 0 — done in spike:** rv32 build, entry symbols, python lowering to a
  valid v3 artifact (header now correct after the local `syscall_abi 0→1` fix),
  and execution through the `LoweredSdkArtifactSmoke` harness (parse + invoke
  first dispatch entry). Still open: running it with real calldata and plumbing a
  CI-built artifact instead of the `RV32_SMOKE_ARTIFACT` env var.
- **Phase 1 — lowerer ported (2026-05-30):** `groundhog-rv32-lower` (lib + bin
  `rv32-lower`) landed in groundhog-rpc at `crates/rv32/lower`, **at
  byte-for-byte output parity** with `scripts/rv32_lower.py` — verified on
  `counter-rv32` (identical `.rv32a` artifact and JSON metadata sidecar). The
  v3 format now lives in the standalone `groundhog-rv32-artifact` crate — the
  single Rust source of truth — with a hermetic synthetic-ELF test suite (happy
  path + every validation/rejection rule) and an opt-in real-ELF parity test
  (`RV32_LOWER_PARITY_ELF`, mirroring the Spike-oracle out-of-default-CI
  posture). Housed in groundhog-rpc for now; self-contained so it can lift to
  its own repo later. The build-time digest cross-check against hogd's
  `artifact.h` remains the cross-repo guard.
- **Phase 2 — hogd producer switched (2026-05-30):** hogd's manual smoke-test
  producer now uses `built_contracts/build_risc.sh`, which wraps
  `scripts/rv32_make_smoke_artifact.sh` and requires exactly `rv32-lower 0.5.6`;
  `scripts/rv32_lower.py` was deleted. One supported implementation from here
  on. The runtime smoke now builds its generated SDK artifact from the
  `rv32_smoke_counter` fixture in the `built_contracts/integration_test_contracts`
  submodule, so hogd consumes the same ITC source boundary as the WASM fixtures.
  `RV32_SMOKE_ARTIFACT` remains a manual override, and
  `-DHOGD_RV32_LOWER_BIN=/path/to/rv32-lower` can point at a non-`PATH` lowerer.
- **Phase 3 (needs Geoff):** method_id-passing + guest-side dispatcher in the
  SDK macro; thin the lowerer to validate+package+hash; optionally cut artifact
  v4. The strip-symbols requirement disappears here.
- **Phase 4:** make `groundhog build --target rv32` the shared golden path for
  RV32 authors and hogd's generated smoke artifact, matching the existing WASM
  integration shape. Keep `rv32-lower` available as the standalone lower-level
  tool for scripts, debugging, and pinned interop checks.

Phases 1–2 are valuable on their own (they kill the two-implementations drift)
and do not block on the runtime-side dispatch decision in Phase 3.
