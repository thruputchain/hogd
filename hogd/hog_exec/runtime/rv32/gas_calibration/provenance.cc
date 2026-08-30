#include "hogd/hog_exec/runtime/rv32/gas_calibration/provenance.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <fstream>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <sys/utsname.h>

#include "hogd/hog_exec/runtime/rv32/gas_calibration/kernel.h"
#include "hogd/hog_exec/runtime/rv32/gas_calibration/lowering_audit.h"
#include "hogd/hog_exec/runtime/rv32/program.h"
#include "hogd/hog_exec/runtime/rv32/test_program_builder.h"
#include "hogd/hog_exec/runtime/rv32/threaded_machine.h"

namespace hogd::rv32::gas_calibration {
namespace {

constexpr uint64_t kFnvOffsetBasis = 0xCBF29CE484222325ULL;
constexpr uint64_t kFnvPrime = 0x100000001B3ULL;

void
mix(uint64_t &hash, uint64_t value)
{
  for (int byte = 0; byte < 8; ++byte) {
    hash ^= (value >> (byte * 8)) & 0xFFULL;
    hash *= kFnvPrime;
  }
}

void
mix(uint64_t &hash, std::string_view text)
{
  for (const char character : text) {
    hash ^= static_cast<unsigned char>(character);
    hash *= kFnvPrime;
  }
}

// One canonical encoding per opcode the harness can place, every register field
// pointed at the same register so that same-register fusion conditions can
// actually be satisfied. Using the calibration kernels' operands here would be
// a mistake in a way that is easy to miss: those are chosen specifically so
// nothing fuses, so a corpus built from them probes a lowering in which no
// fusion rule can fire, and the fingerprint would not move when one changed.
//
// The corpus is fixed, so it is stable across hosts and moves only when the
// opcode set does.
std::vector<uint32_t>
fingerprint_corpus()
{
  std::vector<uint32_t> words;
  for (size_t index = 0; index < kOpCount; ++index) {
    const auto word =
        canonical_instruction_word(static_cast<Op>(index), kDataBankBase);
    if (word.has_value()) {
      words.push_back(*word);
    }
  }
  return words;
}

// Lowers `[filler, head, tail, filler, ecall]` and reports whether the lowering
// rewrote the head slot -- that is, whether head and tail fused. Compared
// against the same head lowered next to a filler, so the answer is about the
// pair and not about the head.
bool
pair_fuses(uint32_t head, uint32_t tail)
{
  const std::vector<uint8_t> paired =
      test_program::code_from_words({filler_instruction_word(0), head, tail,
                                     filler_instruction_word(1), 0x00000073U});
  const std::vector<uint8_t> alone =
      test_program::code_from_words({filler_instruction_word(0), head,
                                     filler_instruction_word(1), 0x00000073U});
  auto paired_program =
      ThreadedMachinePreInstantiation::create(paired, {}, 0, 64,
                                              /*trace=*/false);
  auto alone_program = ThreadedMachinePreInstantiation::create(alone, {}, 0, 64,
                                                               /*trace=*/false);
  if (!paired_program.has_value() || !alone_program.has_value()) {
    return false;
  }
  const std::span<const Slot> paired_slots =
      ThreadedMachinePreInstantiationTestPeer::slots(*paired_program);
  const std::span<const Slot> alone_slots =
      ThreadedMachinePreInstantiationTestPeer::slots(*alone_program);
  if (paired_slots.size() < 2 || alone_slots.size() < 2) {
    return false;
  }
  return paired_slots[1].handler != alone_slots[1].handler;
}

std::string
read_cpu_model()
{
  std::ifstream cpuinfo("/proc/cpuinfo");
  std::string line;
  while (std::getline(cpuinfo, line)) {
    const size_t colon = line.find(':');
    if (colon == std::string::npos) {
      continue;
    }
    const std::string key = line.substr(0, line.find_first_of(" \t"));
    if (key == "model" && line.starts_with("model name")) {
      std::string value = line.substr(colon + 1);
      const size_t start = value.find_first_not_of(" \t");
      return start == std::string::npos ? std::string{} : value.substr(start);
    }
  }
  return {};
}

std::string
compiler_id()
{
#ifdef __clang__
  return "clang " __clang_version__;
#elifdef __GNUC__
  return "gcc " __VERSION__;
#else
  return "unknown";
#endif
}

} // namespace

std::string
engine_fingerprint()
{
  uint64_t hash = kFnvOffsetBasis;

  // The gas schedule: a cost change makes old numbers incomparable even if the
  // engine's code did not move.
  mix(hash, "gas_schedule");
  for (const InstructionGasEntry &entry : kInstructionGasSchedule) {
    mix(hash, reinterpret_cast<const char *>(entry.name));
    mix(hash, entry.cost);
  }

  // Structural limits the measurement depends on.
  mix(hash, "limits");
  mix(hash, kMaxRunGas);
  mix(hash, kMaxMemoryBytes);
  mix(hash, kMaxCodeBytes);
  mix(hash, kRegisterCount);
  mix(hash, kOpCount);

  // The fusion table, probed rather than read: every ordered pair from the
  // canonical corpus, hashed as fused or not.
  mix(hash, "fusion");
  const std::vector<uint32_t> corpus = fingerprint_corpus();
  mix(hash, corpus.size());
  for (const uint32_t head : corpus) {
    for (const uint32_t tail : corpus) {
      mix(hash, pair_fuses(head, tail) ? 1ULL : 0ULL);
    }
  }

  return std::format("{:016x}", hash);
}

Provenance
collect_provenance()
{
  Provenance provenance;

  utsname system = {};
  if (uname(&system) == 0) {
    provenance.host = system.nodename;
    provenance.kernel = std::string(system.sysname) + " " + system.release +
                        " " + system.machine;
  }
  provenance.cpu_model = read_cpu_model();
  provenance.cpu_count = std::thread::hardware_concurrency();

  provenance.compiler = compiler_id();
#ifdef RV32_GAS_BUILD_TYPE
  provenance.build_type = RV32_GAS_BUILD_TYPE;
#endif
#ifdef RV32_GAS_CXX_FLAGS
  provenance.cxx_flags = RV32_GAS_CXX_FLAGS;
#endif
#ifdef __OPTIMIZE__
  provenance.optimized = true;
#endif
#ifndef NDEBUG
  provenance.assertions_enabled = true;
#endif

  if (const char *revision = std::getenv("RV32_GAS_SOURCE_REV");
      revision != nullptr) {
    provenance.source_revision = revision;
  }
  provenance.engine_fingerprint = engine_fingerprint();
  return provenance;
}

} // namespace hogd::rv32::gas_calibration
