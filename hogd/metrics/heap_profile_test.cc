#include "hogd/metrics/heap_profile.h"

#include <cstddef>
#include <cstdint>
#include <exception>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {
void
require(bool condition)
{
  if (!condition)
    throw std::runtime_error("heap profile assertion failed");
}

std::uint64_t
read_varint(std::string_view &input)
{
  std::uint64_t value = 0;
  for (unsigned shift = 0; shift < 64; shift += 7) {
    require(!input.empty());
    unsigned char byte = input.front();
    input.remove_prefix(1);
    value |= std::uint64_t(byte & 127) << shift;
    if (!(byte & 128))
      return value;
  }
  throw std::runtime_error("invalid protobuf varint");
}

struct Field {
  std::uint64_t tag, value;
  std::string_view message;
};

std::vector<Field>
decode(std::string_view input)
{
  std::vector<Field> result;
  while (!input.empty()) {
    const auto key = read_varint(input);
    Field field{key >> 3, 0, {}};
    if ((key & 7) == 0)
      field.value = read_varint(input);
    else {
      require((key & 7) == 2);
      const auto size = read_varint(input);
      require(size <= input.size());
      field.message = input.substr(0, size);
      input.remove_prefix(size);
    }
    result.push_back(field);
  }
  return result;
}

void
invalid(std::string_view text)
{
  bool rejected = false;
  try {
    hogd::jemalloc_to_pprof(text, "");
  } catch (const std::invalid_argument &) {
    rejected = true;
  }
  require(rejected);
}
} // namespace

int
main(int argc, char **argv)
{
  try {
    require(argc == 2);
    std::ifstream fixture(argv[1]);
    require(fixture.good());
    const std::string dump{std::istreambuf_iterator<char>(fixture), {}};
    const auto captured = hogd::jemalloc_to_pprof(dump, "");
    std::size_t captured_samples = 0;
    for (const auto &field : decode(captured)) {
      if (field.tag == 2) {
        ++captured_samples;
        require(!decode(field.message).empty());
      }
    }
    require(captured_samples > 0);

    // Duplicate PCs share IDs; thread rows and summary must not double count.
    const auto profile = hogd::jemalloc_to_pprof(
        "heap_v2/524288\n  t*: 99: 999 [0: 0]\n"
        "@ 0x1011 0x1021\n  t*: 1: 524288 [0: 0]\n"
        "  t0: 1: 524288 [0: 0]\n"
        "@ 0x1011\n  t*: 0: 0 [0: 0]\nMAPPED_LIBRARIES:\n",
        "1000-2000 r-xp 00001000 00:00 1 /tmp/a binary\n");
    auto fields = decode(profile);
    std::size_t samples = 0, locations = 0, types = 0, mappings = 0;
    std::vector<std::string_view> strings;
    for (const auto &field : fields) {
      if (field.tag == 1)
        ++types;
      if (field.tag == 2) {
        ++samples;
        auto sample = decode(field.message);
        require(sample.size() == 4);
        require(sample[0].tag == 1 && sample[0].value == 1);
        require(sample[1].tag == 1 && sample[1].value == 2);
        require(sample[2].tag == 2 && sample[2].value == 2);
        require(sample[3].tag == 2 && sample[3].value == 829411);
      }
      if (field.tag == 3) {
        ++mappings;
        auto mapping = decode(field.message);
        require(mapping.size() == 5);
        require(mapping[1].value == 0x1000 && mapping[2].value == 0x2000);
        require(mapping[3].value == 0x1000 && mapping[4].value == 5);
      }
      if (field.tag == 4) {
        ++locations;
        auto location = decode(field.message);
        require(location.size() == 3 && location[1].value == 1);
        require(location[2].value == 0x1011 || location[2].value == 0x1020);
      }
      if (field.tag == 6)
        strings.push_back(field.message);
    }
    require(types == 2 && samples == 1 && locations == 2 && mappings == 1);
    require(strings.size() == 6 && strings[0].empty());
    require(strings[5] == "/tmp/a binary");
    invalid("heap_v1/1\n");
    invalid("heap_v2/0\n");
    invalid("heap_v2/1\n@ 0x1\n");
    invalid("heap_v2/1\n@ 0x0\n");
    invalid("heap_v2/1\n@ 0x1\n t*: 0: 1 [0: 0]\n");
    invalid("heap_v2/1\n@ 0x1\n t*: 1: 1 [0: 0]\n t*: 1: 1 [0: 0]\n");
    std::cout << "heap profile tests passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
