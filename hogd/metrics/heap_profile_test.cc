#include "hogd/metrics/heap_profile.h"

#include <cstddef>
#include <cstdint>
#include <exception>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <optional>
#include <set>
#ifdef __linux__
#include <ios>
#include <sstream>
#endif
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#ifdef __linux__
namespace heap_profile_test_symbols {
[[gnu::noinline]] static int
known_heap_frame(int value)
{
  return value + 1;
}
} // namespace heap_profile_test_symbols
#endif

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
    value |= static_cast<std::uint64_t>(byte & 127U) << shift;
    if (!(byte & 128U))
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
    Field field{.tag = key >> 3U, .value = 0, .message = {}};
    if ((key & 7U) == 0) {
      field.value = read_varint(input);
    }
    else {
      require((key & 7U) == 2);
      const auto size = read_varint(input);
      require(size <= input.size());
      field.message = input.substr(0, size);
      input.remove_prefix(size);
    }
    result.push_back(field);
  }
  return result;
}

std::uint64_t
value(const std::vector<Field> &fields, std::uint64_t tag)
{
  for (const auto &field : fields)
    if (field.tag == tag)
      return field.value;
  return 0;
}

class FakeSymbolizer : public hogd::HeapSymbolizer {
public:
  std::map<std::string, unsigned> calls;
  std::vector<std::uint64_t> offsets;

  std::string build_id(const std::string &path) override
  {
    ++calls[path];
    return path == "/a" ? "0123abcd" : "";
  }

  std::optional<hogd::HeapSymbol> symbol(const std::string &path,
                                         std::uint64_t offset) override
  {
    offsets.push_back(offset);
    if (path == "/a" && (offset == 0x11 || offset == 0x20))
      return hogd::HeapSymbol{
          .name = "foo::bar(int)",
          .system_name = "_ZN3foo3barEi",
      };
    if (path == "/a" && offset == 0x30)
      return hogd::HeapSymbol{.name = "plain", .system_name = "plain"};
    return std::nullopt;
  }
};

void
symbols_test()
{
  FakeSymbolizer fake;
  auto profile = hogd::jemalloc_to_pprof(
      "heap_v2/524288\n@ 0x1011 0x1021 0x1031 0x3011 0x4011\n"
      "  t*: 1: 524288 [0: 0]\nMAPPED_LIBRARIES:\n",
      "1000-2000 r-xp 0 00:00 1 /a\n"
      "3000-4000 r-xp 1000 00:00 1 /a\n"
      "4000-5000 r-xp 0 00:00 2 /b\n"
      "5000-6000 rw-p 0 00:00 0 [heap]\n"
      "6000-7000 r-xp 0 00:00 3 /gone (deleted)\n",
      fake);
  require(fake.calls.size() == 2 && fake.calls["/a"] == 1 &&
          fake.calls["/b"] == 1);
  require(fake.offsets ==
          std::vector<std::uint64_t>({0x11, 0x20, 0x30, 0x1010, 0x10}));
  auto fields = decode(profile);
  std::vector<std::string_view> strings;
  std::map<std::uint64_t, std::vector<Field>> functions;
  for (const auto &field : fields) {
    if (field.tag == 6)
      strings.push_back(field.message);
    if (field.tag == 5) {
      auto function = decode(field.message);
      require(functions.emplace(value(function, 1), function).second);
    }
  }
  require(strings.front().empty());
  require(std::set<std::string_view>(strings.begin(), strings.end()).size() ==
          strings.size());
  require(functions.size() == 2);
  require(strings[value(functions.at(1), 2)] == "foo::bar(int)");
  require(strings[value(functions.at(1), 3)] == "_ZN3foo3barEi");
  require(strings[value(functions.at(2), 2)] == "plain");
  require(value(functions.at(2), 2) == value(functions.at(2), 3));
  unsigned lines = 0;
  for (const auto &field : fields) {
    if (field.tag == 3) {
      auto mapping = decode(field.message);
      auto id = value(mapping, 1);
      require(value(mapping, 7) == (id == 1 ? 1 : 0));
      if (id <= 2)
        require(strings[value(mapping, 6)] == "0123abcd");
      else
        require(value(mapping, 6) == 0);
    }
    if (field.tag == 4) {
      auto location = decode(field.message);
      auto pc = value(location, 3);
      bool resolved = false;
      for (const auto &frame : location) {
        if (frame.tag != 4)
          continue;
        resolved = true;
        ++lines;
        require(value(decode(frame.message), 1) == (pc == 0x1030 ? 2 : 1));
      }
      require(resolved == (pc < 0x2000));
    }
  }
  require(lines == 3);
}

#ifdef __linux__
void
elf_test()
{
  require(heap_profile_test_symbols::known_heap_frame(4) == 5);
  auto pc = reinterpret_cast<std::uintptr_t>(
                &heap_profile_test_symbols::known_heap_frame) +
            4;
  std::ifstream input("/proc/self/maps");
  require(input.good());
  std::string maps{std::istreambuf_iterator<char>(input), {}};
  std::ostringstream dump;
  dump << "heap_v2/524288\n@ 0x" << std::hex << pc
       << "\n  t*: 1: 524288 [0: 0]\nMAPPED_LIBRARIES:\n";
  auto profile = hogd::jemalloc_to_pprof(dump.str(), maps);
  auto fields = decode(profile);
  std::vector<std::string_view> strings;
  std::uint64_t function_id = 0, mapping_id = 0;
  for (const auto &field : fields)
    if (field.tag == 6)
      strings.push_back(field.message);
  for (const auto &field : fields) {
    if (field.tag == 5) {
      auto function = decode(field.message);
      if (strings[value(function, 2)].find(
              "heap_profile_test_symbols::known_heap_frame") !=
          std::string_view::npos)
        function_id = value(function, 1);
    }
  }
  require(function_id != 0);
  for (const auto &field : fields) {
    if (field.tag == 4) {
      auto location = decode(field.message);
      require(value(location, 3) == pc);
      mapping_id = value(location, 2);
      bool referenced = false;
      for (const auto &frame : location)
        if (frame.tag == 4)
          referenced = value(decode(frame.message), 1) == function_id;
      require(referenced);
    }
  }
  require(mapping_id != 0);
  for (const auto &field : fields) {
    if (field.tag != 3)
      continue;
    auto mapping = decode(field.message);
    if (value(mapping, 1) != mapping_id)
      continue;
    require(pc >= value(mapping, 2) && pc < value(mapping, 3));
    require(value(mapping, 7) == 1 && value(mapping, 6) != 0);
    auto id = strings[value(mapping, 6)];
    require(!id.empty() &&
            id.find_first_not_of("0123456789abcdef") == std::string_view::npos);
  }
}
#endif

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
    symbols_test();
#ifdef __linux__
    elf_test();
#endif

    // Duplicate PCs share IDs; thread rows and summary must not double count.
    FakeSymbolizer empty;
    const auto profile = hogd::jemalloc_to_pprof(
        "heap_v2/524288\n  t*: 99: 999 [0: 0]\n"
        "@ 0x1011 0x1021\n  t*: 1: 524288 [0: 0]\n"
        "  t0: 1: 524288 [0: 0]\n"
        "@ 0x1011\n  t*: 0: 0 [0: 0]\nMAPPED_LIBRARIES:\n",
        "1000-2000 r-xp 00001000 00:00 1 /tmp/a binary\n", empty);
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
