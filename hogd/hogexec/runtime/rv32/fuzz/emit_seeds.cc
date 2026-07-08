#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <span>
#include <string>
#include <vector>

#include "hogd/hogexec/runtime/rv32/artifact.h"
#include "hogd/hogexec/runtime/rv32/fuzz/fuzz_common.h"
#include "hogd/hogexec/runtime/rv32/test_artifact_builder.h"
#include "hogd/hogexec/runtime/rv32/test_corpus.h"
#include "hogd/hogexec/runtime/rv32/test_program_builder.h"

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
// [u64 gas][u32 memory_bytes][code bytes].
std::vector<uint8_t>
engine_diff_seed(const std::vector<uint32_t> &words)
{
  std::vector<uint8_t> seed;
  test_artifact::append_u64(seed, kFuzzGasCap);
  test_artifact::append_u32(seed, kFuzzMemCap);
  const std::vector<uint8_t> code = test_program::code_from_words(words);
  seed.insert(seed.end(), code.begin(), code.end());
  return seed;
}

} // namespace

int
main(int argc, char **argv)
{
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
  const std::vector<uint8_t> valid =
      test_artifact::build_artifact({test_program::addi(1, 0, 5), 0x00000073U},
                                    {});
  write_file(artifact_dir / "valid.bin", valid);

  std::vector<uint8_t> bad_magic = valid;
  bad_magic[0] ^= 0xFF; // corrupt the first magic byte
  write_file(artifact_dir / "bad_magic.bin", bad_magic);

  const std::vector<uint8_t> truncated(
      valid.begin(),
      valid.begin() + static_cast<std::ptrdiff_t>(kArtifactHeaderBytes / 2));
  write_file(artifact_dir / "truncated_header.bin", truncated);

  // Engine-diff seeds: a few fusion-heavy programs from the existing generator.
  for (uint64_t seed = 0; seed < 4; ++seed) {
    const std::vector<uint8_t> bytes =
        engine_diff_seed(test_corpus::generate_engine_only_program(seed));
    write_file(engine_dir / ("engine_" + std::to_string(seed) + ".bin"), bytes);
  }

  std::cerr << "wrote seeds under " << root << "\n";
  return 0;
}
