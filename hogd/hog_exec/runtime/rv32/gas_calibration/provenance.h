#pragma once

#include <cstdint>
#include <string>

namespace hogd::rv32::gas_calibration {

// Host, build, and engine identity for comparing calibration runs.
struct Provenance {
  // Host identity and the CPU whose nanoseconds these are.
  std::string host;
  std::string kernel;
  std::string cpu_model;
  uint32_t cpu_count = 0;

  // Compiler and flags stamped by CMake; optimization and code layout affect
  // interpreter timings even when source and gas costs are identical.
  std::string compiler;
  std::string build_type; // CMAKE_BUILD_TYPE, or empty outside CMake
  std::string cxx_flags;  // the flags this binary was actually built with
  bool optimized = false;
  bool assertions_enabled = false;

  // Calibration requires the project's optimized Release configuration.
  [[nodiscard]] bool is_production_build() const
  {
    return build_type == "Release" && optimized;
  }

  // Source revision, from the RV32_GAS_SOURCE_REV environment variable that
  // scripts/rv32_gas_calibrate.sh fills in from git. Empty when the harness was
  // run without that environment variable.
  std::string source_revision;

  // Behavioural fingerprint of the engine under measurement: see
  // engine_fingerprint() below.
  std::string engine_fingerprint;
  // CMake source digest catches edits the behavioural probe misses, including
  // handler changes and comments. A mismatch need not imply changed timings.
  std::string engine_source_digest;
};

// Hashes the gas schedule, structural limits, and fusion decisions from a
// fixed instruction corpus, without relying on handler addresses. Equal hashes
// do not establish equal performance: handler implementation, compiler, and
// host changes may leave these probes unchanged.
[[nodiscard]] std::string engine_fingerprint();

[[nodiscard]] Provenance collect_provenance();

} // namespace hogd::rv32::gas_calibration
