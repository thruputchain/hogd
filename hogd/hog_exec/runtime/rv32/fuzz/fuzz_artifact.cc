#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

#include "hogd/base/log.h"
#include "hogd/hog_exec/runtime/rv32/artifact.h"
#include "hogd/hog_exec/runtime/rv32/encoding.h"
#include "hogd/hog_exec/runtime/rv32/fuzz/fuzz_common.h"
#include "hogd/hog_exec/runtime/rv32/program.h"
#include "hogd/hog_exec/runtime/rv32/test_artifact_builder.h"
#include "hogd/hog_exec/runtime/rv32/threaded_machine.h"

extern "C" size_t LLVMFuzzerMutate(uint8_t *data, size_t size, size_t max_size);

namespace {

namespace rv32 = hogd::rv32;
namespace fuzz = hogd::rv32::fuzz;

// Repair the artifact envelope so mutations can reach instruction decoding,
// or deliberately break one parser layer while preserving earlier invariants.
class ArtifactMutator {
public:
  ArtifactMutator(std::span<const uint8_t> input, size_t max_size,
                  unsigned int seed)
    : input_(input), max_size_(max_size), random_state_(seed)
  {}

  // Choose raw mutation or one structured strategy, returning bytes in-place
  // as required by LLVMFuzzerCustomMutator.
  size_t mutate(uint8_t *output)
  {
    const size_t strategy = random_inrange(kStrategyCount);
    if (strategy == 0 || max_size_ < kEnvelopeBytes + rv32::kInstructionBytes) {
      return LLVMFuzzerMutate(output, input_.size(), max_size_);
    }

    make_canonical();
    if (artifact_.empty()) {
      return LLVMFuzzerMutate(output, input_.size(), max_size_);
    }

    switch (strategy) {
    case 1:
      mutate_magic();
      break;
    case 2:
      mutate_u32_field(rv32::kArtifactFormatVersionOffset,
                       rv32::kArtifactVersion6);
      break;
    case 3:
      mutate_u32_field(rv32::kArtifactHeaderSizeOffset,
                       rv32::kArtifactHeaderBytes);
      break;
    case 4:
      mutate_u64_field(rv32::kArtifactVmProfileOffset,
                       rv32::kArtifactVmProfileDigest);
      break;
    case 5:
      mutate_u32_field(rv32::kArtifactFlagsOffset,
                       rv32::kArtifactFlagsRequiredValue);
      break;
    case 6:
      mutate_u32_field(rv32::kArtifactEntryAbiOffset,
                       rv32::kArtifactEntryAbiVersion);
      break;
    case 7:
      mutate_u32_field(rv32::kArtifactSyscallAbiOffset,
                       rv32::kArtifactSyscallAbiVersion);
      break;
    case 8:
      mutate_gas_schedule(/*keep_pair=*/true);
      break;
    case 9:
      mutate_gas_schedule(/*keep_pair=*/false);
      break;
    case 10:
      mutate_u32_field(rv32::kArtifactMemoryBytesOffset,
                       load_u32(rv32::kArtifactMemoryBytesOffset));
      break;
    case 11:
      mutate_u32_field(rv32::kArtifactCodeSizeOffset,
                       load_u32(rv32::kArtifactCodeSizeOffset));
      break;
    case 12:
      mutate_u32_field(rv32::kArtifactInitialMemorySizeOffset,
                       load_u32(rv32::kArtifactInitialMemorySizeOffset));
      break;
    case 13:
      mutate_u32_field(rv32::kArtifactInitialMemoryStartOffset,
                       load_u32(rv32::kArtifactInitialMemoryStartOffset));
      break;
    case 14:
      mutate_code(/*repair_digest=*/false);
      break;
    case 15:
      mutate_code(/*repair_digest=*/true);
      break;
    case 16:
      mutate_memory(/*repair_digest=*/false);
      break;
    case 17:
      mutate_memory(/*repair_digest=*/true);
      break;
    case 18:
      inject_invalid_instruction();
      break;
    case 19:
      mutate_code_shape();
      break;
    case 20:
      mutate_memory_shape();
      break;
    case 21:
      mutate_physical_layout();
      break;
    case 22:
      mutate_digest();
      break;
    case 23:
      make_zero_memory_artifact();
      break;
    case 24:
      set_u32(rv32::kArtifactMemoryBytesOffset, rv32::kDefaultMaxMemoryBytes);
      break;
    case 25:
      return mutate_canonical_with_libfuzzer(output);
    default:
      break;
    }
    return write_output(output);
  }

private:
  static constexpr size_t kStrategyCount = 26;
  // Header plus the count of the empty v6 extension list.
  static constexpr size_t kEnvelopeBytes = rv32::kArtifactHeaderBytes + 4;

  uint64_t next_random()
  {
    random_state_ += 0x9e3779b97f4a7c15ULL;
    uint64_t value = random_state_;
    value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ULL;
    value = (value ^ (value >> 27)) * 0x94d049bb133111ebULL;
    return value ^ (value >> 31);
  }

  size_t random_inrange(size_t exclusive_upper)
  {
    return static_cast<size_t>(next_random() % exclusive_upper);
  }

  bool random_flip() { return (next_random() & 1U) != 0; }

  bool random_one_in(size_t denominator)
  {
    return random_inrange(denominator) == 0;
  }

  uint8_t random_byte() { return static_cast<uint8_t>(next_random()); }

  template<typename T> T random_bit()
  {
    return T{1} << random_inrange(std::numeric_limits<T>::digits);
  }

  size_t random_artifact_pos() { return random_inrange(artifact_.size()); }

  // Choose a byte boundary, including the position after the final byte.
  size_t random_artifact_insert_pos()
  {
    return random_inrange(artifact_.size() + 1);
  }

  template<typename T, size_t N> T choose(const std::array<T, N> &values)
  {
    return values[random_inrange(values.size())];
  }

  uint32_t load_u32(size_t offset) const
  {
    return rv32::load_u32_le(std::span<const uint8_t>(artifact_), offset);
  }

  void set_u32(size_t offset, uint32_t value)
  {
    rv32::store_u32_le(std::span<uint8_t>(artifact_), offset, value);
  }

  void set_u64(size_t offset, uint64_t value)
  {
    rv32::store_u64_le(std::span<uint8_t>(artifact_), offset, value);
  }

  // Own a section before rebuild() invalidates spans into artifact_.
  static std::vector<uint8_t> copy_bytes(std::span<const uint8_t> bytes)
  {
    return std::vector<uint8_t>(bytes.begin(), bytes.end());
  }

  // Establish a valid envelope around useful bytes so one later mutation can
  // target a specific parser layer instead of stopping at magic or layout.
  void make_canonical()
  {
    if (canonicalize_existing()) {
      return;
    }

    // carve_artifact_fixup consumes a 12-byte control prefix. Bound its input
    // so replacing that prefix with the artifact envelope still fits
    // libFuzzer's MaxSize.
    const size_t carve_size =
        std::min(input_.size(), max_size_ - kEnvelopeBytes + 12);
    const auto fixup = fuzz::carve_artifact_fixup(input_.first(carve_size));
    if (fixup.has_value()) {
      artifact_ =
          rv32::test_artifact::build_artifact(fixup->code_words,
                                              fixup->initial_memory,
                                              fixup->initial_memory_start);
      set_u32(rv32::kArtifactMemoryBytesOffset, fixup->memory_bytes);
      if (artifact_.size() <= max_size_) {
        return;
      }
    }

    artifact_ =
        rv32::test_artifact::build_artifact({rv32::test_program::addi(0, 0, 0)},
                                            std::span<const uint8_t>{});
  }

  // Recover code and memory from a structurally plausible existing artifact,
  // normalize placement bounds, and replace all other header data and digests.
  bool canonicalize_existing()
  {
    if (input_.size() < rv32::kArtifactHeaderBytes) {
      return false;
    }
    const size_t body_size = input_.size() - rv32::kArtifactHeaderBytes;
    const size_t body_capacity = max_size_ - kEnvelopeBytes;
    const uint32_t code_size =
        rv32::load_u32_le(input_, rv32::kArtifactCodeSizeOffset);
    const uint32_t memory_size =
        rv32::load_u32_le(input_, rv32::kArtifactInitialMemorySizeOffset);
    // Bytes after initial memory are a v6 extension list (none in v5);
    // rebuild() replaces them with an empty one.
    if (code_size == 0 || code_size > body_size ||
        memory_size > body_size - code_size ||
        memory_size > rv32::kDefaultMaxMemoryBytes ||
        code_size > body_capacity || memory_size > body_capacity - code_size) {
      return false;
    }

    const auto code = input_.subspan(rv32::kArtifactHeaderBytes, code_size);
    const auto memory =
        input_.subspan(rv32::kArtifactHeaderBytes + code_size, memory_size);
    uint32_t start =
        rv32::load_u32_le(input_, rv32::kArtifactInitialMemoryStartOffset);
    if (memory.empty()) {
      start = 0;
    }
    else if (start > rv32::kDefaultMaxMemoryBytes - memory_size) {
      start = 0;
    }
    const uint32_t floor = start + memory_size;
    uint32_t total =
        rv32::load_u32_le(input_, rv32::kArtifactMemoryBytesOffset);
    if (total < floor || total > rv32::kDefaultMaxMemoryBytes) {
      total = floor;
    }
    rebuild(code, memory, start, total);
    return artifact_.size() <= max_size_;
  }

  // Serialize a canonical v6 header and body with matching section digests.
  void rebuild(std::span<const uint8_t> code, std::span<const uint8_t> memory,
               uint32_t memory_start, uint32_t memory_bytes)
  {
    artifact_.clear();
    artifact_.reserve(kEnvelopeBytes + code.size() + memory.size());
    rv32::test_artifact::append_bytes(artifact_, rv32::kArtifactMagic);
    rv32::test_artifact::append_u32(artifact_, rv32::kArtifactVersion6);
    rv32::test_artifact::append_u32(artifact_, rv32::kArtifactHeaderBytes);
    rv32::test_artifact::append_u64(artifact_, rv32::kArtifactVmProfileDigest);
    rv32::test_artifact::append_u32(artifact_,
                                    rv32::kArtifactFlagsRequiredValue);
    rv32::test_artifact::append_u32(artifact_, rv32::kArtifactEntryAbiVersion);
    rv32::test_artifact::append_u32(artifact_,
                                    rv32::kArtifactSyscallAbiVersion);
    rv32::test_artifact::append_u32(artifact_, rv32::kGasTableVersion);
    rv32::test_artifact::append_u64(artifact_, rv32::kGasScheduleDigest);
    rv32::test_artifact::append_u32(artifact_, memory_bytes);
    rv32::test_artifact::append_u32(artifact_,
                                    static_cast<uint32_t>(code.size()));
    rv32::test_artifact::append_u32(artifact_,
                                    static_cast<uint32_t>(memory.size()));
    rv32::test_artifact::append_u32(artifact_, memory_start);
    rv32::test_artifact::append_digest(artifact_, rv32::artifact_digest(code));
    rv32::test_artifact::append_digest(artifact_,
                                       rv32::artifact_digest(memory));
    rv32::test_artifact::append_bytes(artifact_, code);
    rv32::test_artifact::append_bytes(artifact_, memory);
    rv32::test_artifact::append_u32(artifact_, 0); // Empty extension list.
  }

  std::span<uint8_t> code()
  {
    return std::span<uint8_t>(artifact_).subspan(
        rv32::kArtifactHeaderBytes, load_u32(rv32::kArtifactCodeSizeOffset));
  }

  std::span<uint8_t> memory()
  {
    const size_t code_size = load_u32(rv32::kArtifactCodeSizeOffset);
    return std::span<uint8_t>(artifact_).subspan(
        rv32::kArtifactHeaderBytes + code_size,
        load_u32(rv32::kArtifactInitialMemorySizeOffset));
  }

  void repair_digest(std::span<const uint8_t> section, size_t offset)
  {
    const rv32::ArtifactDigest digest = rv32::artifact_digest(section);
    std::ranges::copy(digest, artifact_.begin() + offset);
  }

  // Break exactly one magic byte while leaving the rest of the artifact valid.
  void mutate_magic()
  {
    const size_t index = random_inrange(rv32::kArtifactMagic.size());
    artifact_[index] ^= random_bit<uint8_t>();
  }

  // Replace a 32-bit field with parser-relevant boundary values or a bit flip.
  void mutate_u32_field(size_t offset, uint32_t current)
  {
    const std::array<uint32_t, 10> boundaries = {
        0,
        1,
        3,
        4,
        current == 0 ? 1 : current - 1,
        current + 1,
        rv32::kDefaultMaxCodeBytes,
        rv32::kDefaultMaxMemoryBytes,
        rv32::kDefaultMaxMemoryBytes + 1,
        std::numeric_limits<uint32_t>::max(),
    };
    uint32_t value = choose(boundaries);
    if (value == current) {
      value ^= random_bit<uint32_t>();
    }
    set_u32(offset, value);
  }

  // Replace a 64-bit field with parser-relevant boundary values or a bit flip.
  void mutate_u64_field(size_t offset, uint64_t current)
  {
    const std::array<uint64_t, 6> boundaries = {
        0,
        1,
        current - 1,
        current + 1,
        uint64_t{1} << 32,
        std::numeric_limits<uint64_t>::max(),
    };
    uint64_t value = choose(boundaries);
    if (value == current) {
      value ^= random_bit<uint64_t>();
    }
    set_u64(offset, value);
  }

  // Select a current or transitional gas pair; optionally corrupt only its
  // digest to exercise pair-wise validation rather than independent fields.
  void mutate_gas_schedule(bool keep_pair)
  {
    const bool transitional = random_flip();
    const uint32_t version = transitional ? rv32::kTransitionalGasTableVersion
                                          : rv32::kGasTableVersion;
    uint64_t digest = transitional ? rv32::kTransitionalGasScheduleDigest
                                   : rv32::kGasScheduleDigest;
    if (!keep_pair) {
      digest ^= random_bit<uint64_t>();
    }
    set_u32(rv32::kArtifactGasVersionOffset, version);
    set_u64(rv32::kArtifactGasDigestOffset, digest);
  }

  // Flip one code byte, optionally repairing SHA-256 so decoding is reached.
  void mutate_code(bool repair)
  {
    auto section = code();
    section[random_inrange(section.size())] ^= random_bit<uint8_t>();
    if (repair) {
      repair_digest(section, rv32::kArtifactCodeDigestOffset);
    }
  }

  // Flip one memory byte, creating a one-byte image when necessary; optionally
  // repair SHA-256 so image placement and pre-instantiation remain reachable.
  void mutate_memory(bool repair)
  {
    if (memory().empty()) {
      const std::array<uint8_t, 1> byte = {
          random_byte(),
      };
      const std::vector<uint8_t> code_copy = copy_bytes(code());
      rebuild(code_copy, byte, 0, 1);
    }
    auto section = memory();
    section[random_inrange(section.size())] ^= random_bit<uint8_t>();
    if (repair) {
      repair_digest(section, rv32::kArtifactInitialMemoryDigestOffset);
    }
  }

  // Install a known-invalid RV32 word and repair the code digest so rejection
  // happens at canonical or threaded instruction decoding.
  void inject_invalid_instruction()
  {
    auto section = code();
    // Preserve partial instructions for parser validation without selecting a
    // word from an empty range.
    if (section.size() < rv32::kInstructionBytes) {
      return;
    }
    const size_t word_offset =
        random_inrange(section.size() / rv32::kInstructionBytes) *
        rv32::kInstructionBytes;
    const std::array<uint32_t, 4> invalid_words = {
        0x00000000U, // compressed encoding
        0x0000000bU, // unsupported custom opcode
        0x30200073U, // unsupported SYSTEM instruction
        0xffffffffU, // unsupported opcode
    };
    rv32::store_u32_le(section, word_offset, choose(invalid_words));
    repair_digest(section, rv32::kArtifactCodeDigestOffset);
  }

  // Rebuild with empty or non-word-aligned code while keeping layout and
  // digests internally consistent, targeting code-size/program validation.
  void mutate_code_shape()
  {
    std::vector<uint8_t> code_copy = copy_bytes(code());
    const std::vector<uint8_t> memory_copy = copy_bytes(memory());
    const uint32_t start = load_u32(rv32::kArtifactInitialMemoryStartOffset);
    const uint32_t total = load_u32(rv32::kArtifactMemoryBytesOffset);
    if (random_one_in(4)) {
      code_copy.clear();
    }
    else {
      const size_t room = max_size_ - artifact_.size();
      const size_t added = std::min<size_t>(1 + random_inrange(4), room);
      for (size_t i = 0; i < added; ++i) {
        code_copy.push_back(random_byte());
      }
    }
    rebuild(code_copy, memory_copy, start, total);
  }

  // Exercise empty-image canonicality or grow the stored image with a matching
  // digest and total-memory declaration.
  void mutate_memory_shape()
  {
    const std::vector<uint8_t> code_copy = copy_bytes(code());
    std::vector<uint8_t> memory_copy = copy_bytes(memory());
    if (random_flip()) {
      memory_copy.clear();
      rebuild(code_copy, memory_copy, 1, 1);
      return;
    }
    const size_t room = max_size_ - artifact_.size();
    if (room != 0) {
      memory_copy.push_back(random_byte());
    }
    const uint32_t start = load_u32(rv32::kArtifactInitialMemoryStartOffset);
    const uint32_t total = start + static_cast<uint32_t>(memory_copy.size());
    rebuild(code_copy, memory_copy, start, total);
  }

  // Insert an undeclared byte or truncate at an arbitrary byte boundary to
  // violate the exact header || code || memory physical layout.
  void mutate_physical_layout()
  {
    if (random_flip() && artifact_.size() < max_size_) {
      const size_t position = random_artifact_insert_pos();
      artifact_.insert(artifact_.begin() + static_cast<ptrdiff_t>(position),
                       random_byte());
    }
    else if (!artifact_.empty()) {
      artifact_.resize(random_artifact_pos());
    }
  }

  void mutate_digest()
  {
    const size_t base = random_flip()
                            ? rv32::kArtifactCodeDigestOffset
                            : rv32::kArtifactInitialMemoryDigestOffset;
    artifact_[base + random_inrange(rv32::kArtifactDigestBytes)] ^=
        random_bit<uint8_t>();
  }

  void make_zero_memory_artifact()
  {
    const std::vector<uint8_t> code_copy = copy_bytes(code());
    rebuild(code_copy, {}, 0, 0);
  }

  // Let libFuzzer make an unconstrained follow-up edit to a valid envelope.
  size_t mutate_canonical_with_libfuzzer(uint8_t *output)
  {
    const size_t size = write_output(output);
    return LLVMFuzzerMutate(output, size, max_size_);
  }

  size_t write_output(uint8_t *output) const
  {
    CHECK(artifact_.size() <= max_size_);
    std::ranges::copy(artifact_, output);
    return artifact_.size();
  }

  std::span<const uint8_t> input_;
  size_t max_size_;
  uint64_t random_state_;
  std::vector<uint8_t> artifact_;
};

// Exercise both public artifact parsers on the exact fuzzer-provided bytes.
void
parse_raw(std::span<const uint8_t> bytes)
{
  const auto image = rv32::parse_artifact(bytes, rv32::Rv32Limits::defaults());
  const auto threaded =
      rv32::parse_threaded_artifact(bytes, rv32::Rv32Limits::defaults());
  (void)image;
  (void)threaded;
}

// Reinterpret arbitrary bytes as valid sections to keep decode, lowering, and
// image-staging code reachable independently of raw artifact validity.
void
parse_fixed_up(std::span<const uint8_t> bytes)
{
  const auto fixup = fuzz::carve_artifact_fixup(bytes);
  if (!fixup.has_value()) {
    return;
  }

  std::vector<uint8_t> artifact =
      rv32::test_artifact::build_artifact(fixup->code_words,
                                          fixup->initial_memory,
                                          fixup->initial_memory_start);
  // build_artifact stamps memory_bytes = start + size; overwrite it (that field
  // is not digested) to also exercise the zero-extended data tail.
  rv32::store_u32_le(std::span<uint8_t>(artifact),
                     rv32::kArtifactMemoryBytesOffset, fixup->memory_bytes);

  const auto image =
      rv32::parse_artifact(artifact, rv32::Rv32Limits::defaults());
  (void)image;

  // Gate threaded lowering on parse_threaded_artifact, not parse_artifact: the
  // two paths use different decoders, and disagreements are interesting input.
  const auto threaded =
      rv32::parse_threaded_artifact(artifact, rv32::Rv32Limits::defaults());
  if (!threaded.has_value()) {
    return;
  }
  auto pre = rv32::ThreadedMachinePreInstantiation::create(
      threaded->code, threaded->initial_memory, threaded->initial_memory_start,
      threaded->memory_bytes, /*trace=*/false);
  if (!pre.has_value()) {
    return;
  }
  rv32::ThreadedMachine machine(*pre, fuzz::kFuzzGasCap);
  (void)machine;
}

} // namespace

// Feed every input through raw parsing and a structure-preserving fixed-up
// path.
extern "C" int
// NOLINTNEXTLINE(readability-identifier-naming)
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
  const std::span<const uint8_t> bytes(data, size);
  parse_raw(bytes);
  parse_fixed_up(bytes);
  return 0;
}

// Apply deterministic format-aware mutations selected from libFuzzer's seed.
extern "C" size_t
// NOLINTNEXTLINE(readability-identifier-naming)
LLVMFuzzerCustomMutator(uint8_t *data, size_t size, size_t max_size,
                        unsigned int seed)
{
  return ArtifactMutator(std::span<const uint8_t>(data, size), max_size, seed)
      .mutate(data);
}
