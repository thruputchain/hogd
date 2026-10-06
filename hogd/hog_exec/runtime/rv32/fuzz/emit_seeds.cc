#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <span>
#include <string>
#include <vector>

#include "hogd/hog_exec/runtime/rv32/artifact.h"
#include "hogd/hog_exec/runtime/rv32/fuzz/fuzz_common.h"
#include "hogd/hog_exec/runtime/rv32/test_artifact_builder.h"
#include "hogd/hog_exec/runtime/rv32/test_corpus.h"
#include "hogd/hog_exec/runtime/rv32/test_program_builder.h"

namespace {

using hogd::rv32::kArtifactHeaderBytes;
using hogd::rv32::fuzz::kFuzzGasCap;
using hogd::rv32::fuzz::kFuzzMemCap;
namespace test_artifact = hogd::rv32::test_artifact;
namespace test_corpus = hogd::rv32::test_corpus;
namespace test_program = hogd::rv32::test_program;

void
write_file(const std::filesystem::path &path, std::span<const uint8_t> bytes)
{
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out.write(reinterpret_cast<const char *>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
}

// Engine-diff seed layout must match carve_engine_diff_input:
// [u64 gas][u32 memory_bytes][u32 run-gas-cap knob][u64 method_id]
// [u64 host_seed][u32 start_knob][u32 img_len_knob][image bytes][code bytes].
// Knob 0 maps to the production kMaxRunGas cap (see run_gas_cap_from_knob).
// With memory_bytes == kFuzzMemCap and start_knob <= kFuzzMemCap -
// image.size(), the carve maps start_knob/img_len_knob through unchanged, so
// the knobs here are the literal memory_start / image length.
std::vector<uint8_t>
engine_diff_seed(const std::vector<uint32_t> &words, uint32_t run_cap_knob = 0,
                 uint64_t host_seed = 0, std::span<const uint8_t> image = {},
                 uint32_t image_start = 0)
{
  std::vector<uint8_t> seed;
  test_artifact::append_u64(seed, kFuzzGasCap);
  test_artifact::append_u32(seed, kFuzzMemCap);
  test_artifact::append_u32(seed, run_cap_knob);
  test_artifact::append_u64(seed, /*method_id=*/host_seed);
  test_artifact::append_u64(seed, host_seed);
  test_artifact::append_u32(seed, image_start);
  test_artifact::append_u32(seed, static_cast<uint32_t>(image.size()));
  seed.insert(seed.end(), image.begin(), image.end());
  const std::vector<uint8_t> code = test_program::code_from_words(words);
  seed.insert(seed.end(), code.begin(), code.end());
  return seed;
}

} // namespace

int
main(int argc, char **argv)
try {
  if (argc != 2) {
    std::cerr << "usage: " << argv[0] << " <corpus-root>\n";
    return 2;
  }
  const std::filesystem::path root(argv[1]);
  const std::filesystem::path artifact_dir = root / "artifact";
  const std::filesystem::path engine_dir = root / "engine_diff";
  std::filesystem::create_directories(artifact_dir);
  std::filesystem::create_directories(engine_dir);

  // Artifact seeds: one valid, plus two near-misses.
  const std::vector<uint8_t> valid_memory = {0};
  const std::vector<uint8_t> valid =
      test_artifact::build_artifact({test_program::addi(1, 0, 5), 0x00000073U},
                                    valid_memory);
  write_file(artifact_dir / "valid.bin", valid);

  std::vector<uint8_t> bad_magic = valid;
  bad_magic[0] ^= 0xFF; // corrupt the first magic byte
  write_file(artifact_dir / "bad_magic.bin", bad_magic);

  const std::vector<uint8_t> truncated(
      valid.begin(),
      valid.begin() + static_cast<std::ptrdiff_t>(kArtifactHeaderBytes / 2));
  write_file(artifact_dir / "truncated_header.bin", truncated);

  // Engine-diff seeds: a few fusion-heavy programs from the existing
  // generator, with distinct host seeds so the ScriptedHost schedule varies
  // across the starting corpus.
  for (uint64_t seed = 0; seed < 4; ++seed) {
    const std::vector<uint8_t> bytes =
        engine_diff_seed(test_corpus::generate_engine_only_program(seed),
                         /*run_cap_knob=*/0, /*host_seed=*/seed);
    write_file(engine_dir / ("engine_" + std::to_string(seed) + ".bin"), bytes);
  }

  // Image-bearing seed: a seeded initial-memory image at a nonzero start so
  // the image-staging arm of the carve is represented in the corpus.
  const std::vector<uint8_t> image = {0x78, 0x56, 0x34, 0x12};
  write_file(engine_dir / "engine_image.bin",
             engine_diff_seed(test_corpus::generate_engine_only_program(0),
                              /*run_cap_knob=*/0, /*host_seed=*/5, image,
                              /*image_start=*/8));

  // Break-carrier seed: knob 1 maps to the smallest run-gas cap
  // (kFuzzMinRunGasCap, one instruction's worth of the priciest opcode), so
  // this straight line of divs lowers with a carrier every couple of slots,
  // putting the chunk-split refund and deficit seams in the fuzzer's starting
  // coverage; at the production cap they are unreachable within kFuzzGasCap.
  std::vector<uint32_t> carrier_words(48, test_program::op(3, 1, 2, 4, 0x01));
  carrier_words.push_back(0x00000073U); // ecall
  write_file(engine_dir / "engine_carrier.bin",
             engine_diff_seed(carrier_words, /*run_cap_knob=*/1));

  std::cerr << "wrote seeds under " << root << "\n";
  return 0;
} catch (const std::exception &e) {
  std::cerr << "rv32_emit_seeds: " << e.what() << "\n";
  return 1;
} catch (...) {
  std::cerr << "rv32_emit_seeds: unknown error\n";
  return 1;
}
