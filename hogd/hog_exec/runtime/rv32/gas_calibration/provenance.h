#pragma once

#include <cstdint>
#include <string>

namespace hogd::rv32::gas_calibration {

// Everything needed to know whether two calibration runs are comparable.
//
// Costs are a property of a (host, build, engine) triple, not of the ISA, so a
// JSON file that does not say which triple it came from cannot be compared with
// another one -- and the whole point of a calibration harness is that its
// output gets compared: across candidate validator machines, and across
// versions of the interpreter as it changes.
struct Provenance {
  // Host identity and the CPU whose nanoseconds these are.
  std::string host;
  std::string kernel;
  std::string cpu_model;
  uint32_t cpu_count = 0;

  // Build identity. An unoptimized or non-LTO build measures a different
  // interpreter than the node runs, so a run that does not record this cannot
  // be trusted to be measuring production codegen.
  std::string compiler;
  bool optimized = false;
  bool assertions_enabled = false;

  // Source revision, from the RV32_GAS_SOURCE_REV environment variable that
  // scripts/rv32_gas_calibrate.sh fills in from git. Empty when the harness was
  // built some other way.
  std::string source_revision;

  // Behavioural fingerprint of the engine under measurement: see
  // engine_fingerprint() below. This is the field that answers "is this number
  // still about the interpreter I have now".
  std::string engine_fingerprint;
};

// A hash over what the production lowering *does*, not over its source text.
//
// Specifically: the instruction gas schedule, the engine's structural limits,
// and -- the part that matters -- which ordered instruction pairs the lowering
// fuses, probed by lowering every pair from a canonical instruction corpus and
// recording whether the head slot came back rewritten.
//
// Deriving it from behaviour rather than from a source digest is deliberate.
// A source hash changes when a comment does and misses a fusion rule that
// changes behaviour through a header the digest did not cover. This changes
// exactly when the measured engine changes in a way that can move the numbers:
// a new or removed fusion rule, a changed gas cost, a changed run-splitting
// cap. Reformat the interpreter and it stays put; add a fusion rule and it
// moves, which is the signal to re-run before trusting an old calibration.
//
// It is also independent of build layout -- it records whether a pair fused,
// never a handler address -- so the same engine fingerprints identically across
// hosts, compilers, and ASLR. That is what lets it distinguish "different
// machine" from "different engine" when comparing two runs.
[[nodiscard]] std::string engine_fingerprint();

[[nodiscard]] Provenance collect_provenance();

} // namespace hogd::rv32::gas_calibration
