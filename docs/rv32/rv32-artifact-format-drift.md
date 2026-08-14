# Making RV32 artifact-format drift explicit

**Status:** design (2026-05-30). Closes the still-open half of Phase 1 in
[rv32-author-toolchain.md](rv32-author-toolchain.md): a build-time digest
cross-check that keeps hogd's `artifact.h` and the Rust artifact crate's
`format.rs` honest.

## Problem

The RV32 **v4 artifact format** is defined twice:

- **C++** — `hogd/hog_exec/runtime/rv32/artifact.h`: the constants, field
  offsets, and validation the runtime applies when it parses an artifact.
- **Rust** — `groundhog-rpc:crates/rv32/artifact/src/format.rs`: the
  constants and writer the lowerer uses to emit the artifact.

The two are kept in sync **by hand**. Nothing stops them drifting: bump an
offset, an ABI version, or a max size on one side and the mismatch only surfaces
later as an artifact that mysteriously fails to load (or, worse, loads when it
should not). We want any v4 drift to be **intentional and visible** — caught at
build time, on the machine of whoever changed it.

This is not about freezing v4. When a future format lands, re-pinning the
fingerprint simply *becomes part of the format bump* — which is exactly the
friction we want.

## Goal / non-goals

**Goal.** A format fingerprint that both languages compute independently and
that fails the build when they disagree, plus a cross-repo proof that the two
fingerprints actually match — not just that two hand-copied literals are equal.

**Non-goals.**
- Not freezing or versioning-up the format.
- Not the crate extraction itself — that is the follow-up slice, de-risked by
  this one (see Sequencing).
- Not generating one definition from the other (codegen was considered and
  rejected as disproportionate for ~15 rarely-changing constants; it would also
  invert `artifact.h`'s clean hand-authored ownership).

## Background: the idiom this generalizes

`artifact.h` already does this for the gas schedule:

```cpp
inline constexpr uint64_t kGasScheduleDigest = gas_schedule_digest(); // FNV-1a fold
static_assert(kGasScheduleDigest == 0x918317c21c46bb60ULL);
```

and `format.rs` mirrors the literal (`GAS_SCHEDULE_DIGEST = 0x9183_17c2_1c46_bb60`).
For the gas-table v3 bump, the Rust side is the `groundhog-rv32-lower` 0.5.7 /
`groundhog-rv32-artifact` crate release, which also pins
`FORMAT_FINGERPRINT = 0x3aed_0191_5d1a_5e1d`. `gas_schedule_digest()` folds
**named values** (versions, op costs, name strings) via FNV-1a with explicit
encoding — it never hashes raw struct bytes. We generalize that idiom from "the
gas schedule" to "the whole format."

## Design

### 1. The format fingerprint is a *contract* digest, not a layout digest

Hash **named fields in a fixed order with explicit little-endian encoding** —
never C++ struct bytes (padding/layout would never match Rust). Fold via FNV-1a,
identical algorithm both sides, mirroring `gas_schedule_digest()`.

Prefix the fold with a **domain string**: `groundhog.rv32.artifact.format.v4`.
This namespaces the digest (no semantic collision with other FNV digests) and
makes the version explicit — when the next format arrives the prefix bumps *and* the
constants change, so the version shift double-signals.

### 2. What gets folded — an objective rule

Fold **exactly what `artifact.cc` validates at load**, in a fixed field order:

- magic (`GHRV32A1`)
- format version (4)
- header size (132)
- vm-profile digest
- flags policy (currently: flags must be 0)
- entry ABI version (2)
- syscall ABI version (1)
- gas-table version (3) and gas-schedule digest
- every header field offset and size (the `kArtifact*Offset` set)
- section digest algorithm (SHA-256) and digest size (32)
- max code / memory sizes
- policy invariants the loader enforces: flags required value = 0, canonical
  section order (`header | code | initial_memory`), no gaps/trailing bytes,
  `initial_memory_size <= memory_bytes`, and `instruction_bytes = 4`

**Explicitly excluded:** metadata-only provenance fields that the runtime never
inspects — `rustc_version`, `build_flags`, `target_triple`. Folding those would
churn the fingerprint on every toolchain bump for no interop benefit.

The "what `artifact.cc` validates" rule is the source of truth for the set, so
the field list is auditable rather than hand-curated by taste.

*Implementation note:* the Rust writer currently derives offsets implicitly
(`code_offset = HEADER_BYTES`, `initial_memory_offset = code_offset + code.len()`,
…) and has no named offset constants. Folding the layout forces writing the
field table down explicitly in Rust, in the same order as the C++ side. Small
work, net-positive legibility.

### 3. Two-layer proof

The shared literal alone is still copy-paste state. The interop check is what
turns it into a real proof. Both layers ship together:

**Layer A — local, compile-time, per repo.** Each side computes the fingerprint
and asserts it against one shared literal:

```cpp
// hogd artifact.h
inline constexpr uint64_t kArtifactFormatFingerprint = format_fingerprint();
static_assert(kArtifactFormatFingerprint == 0x<LITERAL>ULL);
```

```rust
// groundhog-rv32-artifact format.rs
pub const FORMAT_FINGERPRINT: u64 = 0x<LITERAL>;
const _: () = assert!(compute_format_fingerprint() == FORMAT_FINGERPRINT);
```

Change any folded constant on either side → that side's fingerprint moves → its
assert fails to compile until the literal is updated. Layer A makes local drift
intentional; Layer B proves the currently consumed lowerer/runtime pair agree.

**Layer B — cross-repo interop proof, rides the smoke path.** `rv32-lower` gains
`--print-format-fingerprint` and **stamps `FORMAT_FINGERPRINT` into the
`.rv32.json` metadata** it already emits under `artifact_format_fingerprint`.
hogd's smoke test reads the stamped value back and asserts it equals the C++
`kArtifactFormatFingerprint`. This is the step that proves the two repos agree
(vs. trusting a copied number), and it is nearly free — it reuses the
smoke-artifact plumbing landed in #2434.

Unlike the informational `LoweredSdkArtifactSmoke` body (EXPECT, not ASSERT),
the fingerprint check is a **hard failure** — drift detection is the whole
point. It lives as its own assertion/test, not folded into the informational
execution smoke.

## Sequencing

1. **Slice 1 — the cross-check before extraction** (this design).
   hogd `artifact.h` fingerprint + `static_assert`; Rust fingerprint +
   `rv32-lower --print-format-fingerprint` + metadata stamp; hogd smoke
   metadata assertion.
   Touches **hogd** and **groundhog-rpc** only. Front-loads the value; the hogd
   PR still consumes a published `rv32-lower`, so the normal lowerer publish and
   wrapper-version bump happen before the hogd CI path can require the metadata
   field.

2. **Slice 2 — extract `groundhog-rv32-artifact`** (follow-up). Lift the format
   module out of `rv32-lower` into a standalone crate in groundhog-rpc under
   `crates/rv32/` (grouped with `rv32-lower`), not protocol-rs — protocol-rs is the x23c-generated XDR bindings, and the artifact format is a bespoke binary format owned by the
   rv32 tooling. Repoint `rv32-lower` at it. The
   fingerprint from Slice 1 becomes the **regression guard for the extraction**:
   lift the module, fingerprint stays identical ⇒ proof the refactor preserved
   the format. You already know exactly what the canonical crate must preserve.

## Components by repo (Slice 1)

- **hogd** (`hogd/hog_exec/runtime/rv32/artifact.h`): `format_fingerprint()`
  constexpr + `kArtifactFormatFingerprint` + `static_assert`; smoke test
  (`rv32_runtime_context_test.cc`) reads the stamped metadata fingerprint and
  asserts equality with the C++ value.
- **groundhog-rpc** (`crates/rv32/artifact` + `crates/rv32/lower`):
  `compute_format_fingerprint()` + `FORMAT_FINGERPRINT` const + self-assert
  in `artifact/src/format.rs`; explicit Rust field-offset table;
  `rv32-lower --print-format-fingerprint`; stamp the fingerprint into the
  `.rv32.json` metadata writer.

## Testing

- Compile-time: both `static_assert`/`const _` guards (no runtime cost).
- Rust unit test: `compute_format_fingerprint()` returns the pinned literal;
  golden-value test so the literal cannot silently move.
- hogd test: stamped-metadata fingerprint == `kArtifactFormatFingerprint`
  (hard assert). Anchors that the two fold algorithms agree at a concrete point.
- Negative check (Rust unit test): perturbing a folded constant changes the
  fingerprint (guards against a no-op fold bug that would make the digest
  insensitive to drift).

## Risks / open questions

- **Fold-algorithm parity.** The C++ and Rust folds must encode the same fields
  identically. This is a one-time correctness concern, not ongoing drift: the
  golden value plus the Layer-B interop check pin it down on day one. The
  existing `gas_schedule_digest()` already demonstrates cross-language FNV parity
  in this tree.
- **v4 timing (Geoff-owned).** The fingerprint pins the v3 layout. If v4 is
  imminent we simply re-pin when it lands; the value here is auditability, not
  permanence. Confirmed acceptable in review — re-pinning is the desired friction
  at a format bump.
- **Metadata as the interop channel.** Layer B depends on the lowerer stamping
  the fingerprint into metadata and the smoke producing that metadata in CI
  (it does, post-#2434). If the smoke path is ever disabled, Layer B goes with
  it — Layer A still catches local drift.

## Acceptance criteria

1. Editing any folded constant on **either** side fails the build until the
   shared literal is updated.
2. A lowerer/runtime fingerprint mismatch fails CI via the smoke metadata check.
3. Metadata-only provenance fields do **not** affect the fingerprint.
4. The pinned literal is covered by a golden test on the Rust side.
5. The fingerprint is unchanged by the later crate extraction (Slice 2 guard).
