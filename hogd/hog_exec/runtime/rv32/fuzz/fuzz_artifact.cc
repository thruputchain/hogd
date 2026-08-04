#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "hogd/hog_exec/runtime/rv32/artifact.h"
#include "hogd/hog_exec/runtime/rv32/encoding.h"
#include "hogd/hog_exec/runtime/rv32/fuzz/fuzz_common.h"
#include "hogd/hog_exec/runtime/rv32/test_artifact_builder.h"
#include "hogd/hog_exec/runtime/rv32/threaded_machine.h"

namespace {

using hogd::rv32::kArtifactMemoryBytesOffset;
using hogd::rv32::parse_artifact;
using hogd::rv32::parse_threaded_artifact;
using hogd::rv32::store_u32_le;
using hogd::rv32::ThreadedMachine;
using hogd::rv32::ThreadedMachinePreInstantiation;
using hogd::rv32::test_artifact::build_artifact;
namespace fuzz = hogd::rv32::fuzz;

void
parse_raw(std::span<const uint8_t> bytes)
{
  const auto image = parse_artifact(bytes);
  const auto threaded = parse_threaded_artifact(bytes);
  (void)image;
  (void)threaded;
}

void
parse_fixed_up(std::span<const uint8_t> bytes)
{
  const auto fixup = fuzz::carve_artifact_fixup(bytes);
  if (!fixup.has_value()) {
    return;
  }

  std::vector<uint8_t> artifact =
      build_artifact(fixup->code_words, fixup->initial_memory,
                     fixup->initial_memory_start);
  // build_artifact stamps memory_bytes = start + size; overwrite it (that field
  // is not digested) to also exercise the zero-extended data tail.
  store_u32_le(std::span<uint8_t>(artifact), kArtifactMemoryBytesOffset,
               fixup->memory_bytes);

  const auto image = parse_artifact(artifact);
  (void)image;

  // Gate threaded lowering on parse_threaded_artifact, not parse_artifact: the
  // two paths use different decoders, and disagreements are interesting input.
  const auto threaded = parse_threaded_artifact(artifact);
  if (!threaded.has_value()) {
    return;
  }
  auto pre = ThreadedMachinePreInstantiation::create(
      threaded->code, threaded->initial_memory, threaded->initial_memory_start,
      threaded->memory_bytes, /*trace=*/false);
  if (!pre.has_value()) {
    return;
  }
  ThreadedMachine machine(*pre, fuzz::kFuzzGasCap);
  (void)machine;
}

} // namespace

extern "C" int
// NOLINTNEXTLINE(readability-identifier-naming)
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
  const std::span<const uint8_t> bytes(data, size);
  parse_raw(bytes);
  parse_fixed_up(bytes);
  return 0;
}
