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

  // Build identity. The measured engine is the *compiled* interpreter, and the
  // project's Release build differs from a casual optimized build in ways that
  // reach the hot path: -O3 rather than -O2, -march, libc++ under clang, LTO
  // across the interpreter's translation units. So the flags are recorded
  // verbatim, stamped in by CMake, rather than assumed.
  std::string compiler;
  std::string build_type; // CMAKE_BUILD_TYPE, or empty outside CMake
  std::string cxx_flags;  // the flags this binary was actually built with
  bool optimized = false;
  bool assertions_enabled = false;

  // True when this binary was built by the project's Release configuration --
  // the only build whose numbers describe the interpreter the node runs.
  [[nodiscard]] bool is_production_build() const
  {
    return build_type == "Release" && optimized;
  }

  // Source revision, from the RV32_GAS_SOURCE_REV environment variable that
  // scripts/rv32_gas_calibrate.sh fills in from git. Empty when the harness was
  // built some other way.
  std::string source_revision;

  // Behavioural fingerprint of the engine under measurement: see
  // engine_fingerprint() below.
  std::string engine_fingerprint;
  // Digest of the interpreter's source, stamped in by CMake. The behavioural
  // fingerprint catches what the lowering *does* differently; this catches an
  // edit that changes how it does it -- a handler body, the dispatch path, the
  // slot layout -- which moves timings while leaving the fusion matrix, gas
  // table and limits identical. Empty outside CMake.
  std::string engine_source_digest;
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
//
// What it does NOT catch is an edit that changes how the lowering does its
// work without changing what it decides: a rewritten handler body, a different
// dispatch path, a reordered slot. Those move timings and leave this value
// alone, so Provenance::engine_source_digest covers them separately.
[[nodiscard]] std::string engine_fingerprint();

[[nodiscard]] Provenance collect_provenance();

} // namespace hogd::rv32::gas_calibration
