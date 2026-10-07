#include "hogd/metrics/heap_profile.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cxxabi.h>
#include <fstream>
#include <ios>
#include <istream>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <utility>
#include <vector>

namespace hogd {
namespace {
using Number = std::uint64_t;

bool
file_path(const std::string &path)
{
  return path.starts_with('/') && !path.ends_with(" (deleted)");
}

template<typename T>
T
elf_field(std::string_view bytes, std::size_t offset)
{
  T value{};
  if (offset > bytes.size() || sizeof(T) > bytes.size() - offset)
    throw std::runtime_error("short ELF field");
  std::memcpy(&value, bytes.data() + offset, sizeof(T));
  if constexpr (std::endian::native != std::endian::little) {
    T swapped{};
    for (std::size_t i = 0; i < sizeof(T); ++i) {
      swapped = static_cast<T>((swapped << 8U) | (value & 255U));
      value >>= 8U;
    }
    value = swapped;
  }
  return value;
}

struct ElfSegment {
  Number offset, size, address;
};
struct ElfSymbol {
  Number address, size;
  std::string name;
};
struct ElfFile {
  std::string build_id;
  std::vector<ElfSegment> loads;
  std::vector<ElfSymbol> symbols;
};

class ElfReader {
public:
  ElfReader(const std::string &path, Number size)
    : input_(path, std::ios::binary), size_(size)
  {}

  std::string read(Number offset, Number size)
  {
    if (offset > size_ || size > size_ - offset ||
        size >
            static_cast<Number>(std::numeric_limits<std::streamsize>::max()) ||
        offset >
            static_cast<Number>(std::numeric_limits<std::streamoff>::max()))
      throw std::runtime_error("invalid ELF range");
    std::string bytes(size, '\0');
    input_.seekg(static_cast<std::streamoff>(offset));
    if (!input_.read(bytes.data(), static_cast<std::streamsize>(size)))
      throw std::runtime_error("short ELF read");
    return bytes;
  }

private:
  std::ifstream input_;
  Number size_;
};

void
elf_notes(ElfFile &file, std::string_view notes)
{
  while (notes.size() >= 12) {
    Number names = elf_field<std::uint32_t>(notes, 0);
    Number descriptor = elf_field<std::uint32_t>(notes, 4);
    auto type = elf_field<std::uint32_t>(notes, 8);
    notes.remove_prefix(12);
    Number padded_name = (names + 3) & ~Number{3};
    Number padded_descriptor = (descriptor + 3) & ~Number{3};
    if (padded_name > notes.size() ||
        padded_descriptor > notes.size() - padded_name)
      return;
    if (names == 4 && notes.substr(0, 4) == std::string_view("GNU\0", 4) &&
        type == 3) {
      constexpr std::string_view kHex = "0123456789abcdef";
      for (Number i = 0; i < descriptor; ++i) {
        auto byte = static_cast<unsigned char>(notes[padded_name + i]);
        file.build_id += kHex[byte >> 4U];
        file.build_id += kHex[byte & 15U];
      }
      return;
    }
    notes.remove_prefix(padded_name + padded_descriptor);
  }
}

ElfFile
parse_elf(const std::string &path, Number size)
{
  ElfReader reader(path, size);
  auto header = reader.read(0, 64);
  if (header.substr(0, 4) != std::string("\177ELF", 4) || header[4] != 2 ||
      header[5] != 1)
    return {};
  ElfFile file;
  auto phoff = elf_field<Number>(header, 32),
       shoff = elf_field<Number>(header, 40);
  auto phsize = elf_field<std::uint16_t>(header, 54);
  auto phcount = elf_field<std::uint16_t>(header, 56);
  auto shsize = elf_field<std::uint16_t>(header, 58);
  auto shcount = elf_field<std::uint16_t>(header, 60);
  auto table = [&](Number offset, Number stride, Number count, Number minimum) {
    if (count && stride < minimum)
      throw std::runtime_error("short ELF header entry");
    return reader.read(offset, stride * count);
  };
  auto programs = table(phoff, phsize, phcount, 56);
  for (Number i = 0; i < phcount; ++i) {
    auto entry = std::string_view(programs).substr(i * phsize, phsize);
    auto type = elf_field<std::uint32_t>(entry, 0);
    ElfSegment segment{
        .offset = elf_field<Number>(entry, 8),
        .size = elf_field<Number>(entry, 32),
        .address = elf_field<Number>(entry, 16),
    };
    if (segment.offset > size || segment.size > size - segment.offset)
      throw std::runtime_error("invalid ELF segment");
    if (type == 1)
      file.loads.push_back(segment);
    if (type == 4 && (!shoff || !shcount) && file.build_id.empty())
      elf_notes(file, reader.read(segment.offset, segment.size));
  }
  auto sections = table(shoff, shsize, shcount, 64);
  auto section = [&](Number index) {
    if (index >= shcount)
      throw std::runtime_error("invalid ELF section index");
    return std::string_view(sections).substr(index * shsize, shsize);
  };
  std::optional<Number> symtab, dynsym;
  for (Number i = 0; i < shcount; ++i) {
    auto entry = section(i);
    auto type = elf_field<std::uint32_t>(entry, 4);
    if (type == 7 && file.build_id.empty())
      elf_notes(file, reader.read(elf_field<Number>(entry, 24),
                                  elf_field<Number>(entry, 32)));
    if (type == 2 && !symtab)
      symtab = i;
    if (type == 11 && !dynsym)
      dynsym = i;
  }
  auto selected = symtab ? symtab : dynsym;
  if (!selected)
    return file;
  auto entry = section(*selected);
  auto stride = elf_field<Number>(entry, 56);
  if (stride < 24)
    throw std::runtime_error("short ELF symbol entry");
  auto symbols =
      reader.read(elf_field<Number>(entry, 24), elf_field<Number>(entry, 32));
  auto strings_entry = section(elf_field<std::uint32_t>(entry, 40));
  if (elf_field<std::uint32_t>(strings_entry, 4) != 3)
    throw std::runtime_error("invalid ELF string table");
  auto strings = reader.read(elf_field<Number>(strings_entry, 24),
                             elf_field<Number>(strings_entry, 32));
  for (Number i = 0; i < symbols.size() / stride; ++i) {
    auto symbol = std::string_view(symbols).substr(i * stride, stride);
    auto type = static_cast<unsigned char>(symbol[4]) & 15U;
    auto address = elf_field<Number>(symbol, 8);
    Number name = elf_field<std::uint32_t>(symbol, 0);
    if ((type != 2 && type != 10) || !elf_field<std::uint16_t>(symbol, 6) ||
        !address || name >= strings.size())
      continue;
    auto end = strings.find('\0', name);
    if (end == std::string::npos || end == name)
      continue;
    file.symbols.push_back({
        .address = address,
        .size = elf_field<Number>(symbol, 16),
        .name = strings.substr(name, end - name),
    });
  }
  std::ranges::sort(file.symbols, [](const auto &a, const auto &b) {
    return std::pair{a.address, a.name} < std::pair{b.address, b.name};
  });
  return file;
}

class ElfHeapSymbolizer : public HeapSymbolizer {
public:
  std::string build_id(const std::string &path) override
  {
    auto file = get(path);
    return file ? file->build_id : std::string{};
  }

  std::optional<HeapSymbol> symbol(const std::string &path,
                                   Number offset) override
  {
    auto file = get(path);
    if (!file)
      return std::nullopt;
    for (const auto &load : file->loads) {
      if (offset < load.offset || offset - load.offset >= load.size ||
          offset - load.offset >
              std::numeric_limits<Number>::max() - load.address)
        continue;
      Number address = offset - load.offset + load.address;
      auto it = std::upper_bound(file->symbols.begin(), file->symbols.end(),
                                 address, [](Number pc, const auto &sym) {
                                   return pc < sym.address;
                                 });
      if (it == file->symbols.begin())
        return std::nullopt;
      --it;
      if (it->size ? address - it->address >= it->size
                   : std::next(it) == file->symbols.end())
        return std::nullopt;
      int status = 0;
      std::unique_ptr<char, decltype(&std::free)> demangled(
          abi::__cxa_demangle(it->name.c_str(), nullptr, nullptr, &status),
          &std::free);
      return HeapSymbol{
          .name = status == 0 && demangled ? demangled.get() : it->name,
          .system_name = it->name,
      };
    }
    return std::nullopt;
  }

private:
  struct Cached {
    struct stat metadata;
    std::shared_ptr<const ElfFile> file;
  };
  std::map<std::string, std::shared_ptr<const ElfFile>> files_;

  std::shared_ptr<const ElfFile> get(const std::string &path)
  {
    try {
      if (!file_path(path))
        return {};
      if (auto it = files_.find(path); it != files_.end())
        return it->second;
      static std::mutex mutex;
      static std::map<std::string, Cached> cache;
      std::lock_guard<std::mutex> lock(mutex);
      struct stat metadata{};
      if (stat(path.c_str(), &metadata) != 0 || metadata.st_size < 0)
        return files_[path] = {};
      auto it = cache.find(path);
      if (it == cache.end() || it->second.metadata.st_dev != metadata.st_dev ||
          it->second.metadata.st_ino != metadata.st_ino ||
          it->second.metadata.st_size != metadata.st_size ||
          it->second.metadata.st_mtime != metadata.st_mtime) {
        std::shared_ptr<const ElfFile> file;
        try {
          file = std::make_shared<ElfFile>(parse_elf(path, metadata.st_size));
        } catch (...) {
          // Bad or unreadable files are unknown, including on non-ELF hosts.
          file.reset();
        }
        it = cache
                 .insert_or_assign(path,
                                   Cached{
                                       .metadata = metadata,
                                       .file = std::move(file),
                                   })
                 .first;
      }
      return files_[path] = it->second.file;
    } catch (...) {
      return {};
    }
  }
};

void
varint(std::string &out, Number value)
{
  while (value >= 128) {
    out += static_cast<char>((value & 127U) | 128U);
    value >>= 7U;
  }
  out += static_cast<char>(value);
}

void
integer(std::string &out, Number field, Number value)
{
  varint(out, field << 3U);
  varint(out, value);
}

void
message(std::string &out, Number field, std::string_view value)
{
  varint(out, (field << 3U) | 2U);
  varint(out, value.size());
  out += value;
}

Number
number(const std::string &text, int base = 10)
{
  if (text.empty() || text.front() == '-')
    throw std::invalid_argument("invalid heap profile number");
  std::size_t end = 0;
  Number value = std::stoull(text, &end, base);
  if (end != text.size())
    throw std::invalid_argument("invalid heap profile number");
  return value;
}

struct Mapping {
  Number start, limit, offset, filename;
  Number build_id = 0;
  bool has_functions = false;
};

Number
adjusted(Number count, Number bytes, Number period)
{
  if (!count || !bytes)
    return 0;
  const double probability =
      -std::expm1(-static_cast<double>(bytes) / static_cast<double>(count) /
                  static_cast<double>(period));
  const double value = static_cast<double>(bytes) / probability;
  if (!std::isfinite(value) ||
      value >= static_cast<double>(std::numeric_limits<std::int64_t>::max()))
    throw std::invalid_argument("heap profile sample overflow");
  return static_cast<Number>(std::round(value));
}
} // namespace

std::string
jemalloc_to_pprof(std::string_view dump, std::string_view maps)
{
  ElfHeapSymbolizer symbolizer;
  return jemalloc_to_pprof(dump, maps, symbolizer);
}

std::string
jemalloc_to_pprof(std::string_view dump, std::string_view maps,
                  HeapSymbolizer &symbolizer)
{
  std::istringstream input{std::string(dump)};
  std::string line;
  if (!std::getline(input, line) || !line.starts_with("heap_v2/"))
    throw std::invalid_argument("expected jemalloc heap_v2");
  const Number period = number(line.substr(8));
  if (!period)
    throw std::invalid_argument("zero heap sampling period");

  std::vector<std::string> strings{
      "", "inuse_objects", "count", "inuse_space", "bytes",
  };
  std::vector<Mapping> mappings;
  std::map<std::string, Number> string_ids;
  for (std::size_t i = 0; i < strings.size(); ++i)
    string_ids.emplace(strings[i], i);
  auto intern = [&](const std::string &value) {
    auto [it, inserted] = string_ids.try_emplace(value, strings.size());
    if (inserted)
      strings.push_back(value);
    return it->second;
  };
  std::map<std::string, Number> build_ids;
  std::istringstream map_input{std::string(maps)};
  while (std::getline(map_input, line)) {
    std::istringstream fields(line);
    std::string range, permissions, offset, device, inode, filename;
    if (!(fields >> range >> permissions >> offset >> device >> inode))
      throw std::invalid_argument("invalid proc maps entry");
    const auto dash = range.find('-');
    if (dash == std::string::npos)
      throw std::invalid_argument("invalid proc maps range");
    std::getline(fields >> std::ws, filename);
    Number start = number(range.substr(0, dash), 16);
    Number limit = number(range.substr(dash + 1), 16);
    if (start >= limit)
      throw std::invalid_argument("invalid proc maps limits");
    Number filename_id = intern(filename);
    auto [it, inserted] = build_ids.try_emplace(filename, 0);
    if (inserted && file_path(filename))
      it->second = intern(symbolizer.build_id(filename));
    mappings.push_back({
        .start = start,
        .limit = limit,
        .offset = number(offset, 16),
        .filename = filename_id,
        .build_id = it->second,
    });
  }

  std::string profile;
  for (auto [type, unit] : {std::pair{1, 2}, std::pair{3, 4}}) {
    std::string value_type;
    integer(value_type, 1, type);
    integer(value_type, 2, unit);
    message(profile, 1, value_type);
  }
  std::map<Number, Number> locations;
  std::vector<Number> stack;
  bool consumed = true;
  while (std::getline(input, line)) {
    if (line == "MAPPED_LIBRARIES:")
      break;
    if (line.starts_with("@ ")) {
      if (!consumed)
        throw std::invalid_argument("stack without aggregate");
      stack.clear();
      consumed = false;
      std::istringstream frames(line.substr(2));
      std::string address;
      while (frames >> address) {
        // Match jeprof: the leaf is exact; caller PCs are return addresses.
        Number pc = number(address, 16);
        if (!pc)
          throw std::invalid_argument("zero frame address");
        if (!stack.empty())
          --pc;
        auto [it, inserted] = locations.try_emplace(pc, locations.size() + 1);
        stack.push_back(it->second);
      }
      if (stack.empty())
        throw std::invalid_argument("empty heap stack");
    }
    else {
      std::istringstream fields(line);
      std::string tag, count, bytes, cumulative_count, cumulative_bytes;
      if (!(fields >> tag) || tag != "t*:")
        continue; // Per-thread rows and the header summary are not samples.
      if (stack.empty())
        continue;
      if (consumed ||
          !(fields >> count >> bytes >> cumulative_count >> cumulative_bytes) ||
          !count.ends_with(':') || !cumulative_count.starts_with('[') ||
          !cumulative_count.ends_with(':') || !cumulative_bytes.ends_with(']'))
        throw std::invalid_argument("invalid heap aggregate");
      consumed = true;
      Number n = number(count.substr(0, count.size() - 1));
      Number b = number(bytes);
      number(cumulative_count.substr(1, cumulative_count.size() - 2));
      number(cumulative_bytes.substr(0, cumulative_bytes.size() - 1));
      if ((n == 0) != (b == 0))
        throw std::invalid_argument("inconsistent heap sample");
      if (!n)
        continue;
      Number scaled_bytes = adjusted(n, b, period);
      auto scaled_count = static_cast<Number>(
          std::round(static_cast<double>(scaled_bytes) *
                     static_cast<double>(n) / static_cast<double>(b)));
      std::string sample;
      for (Number id : stack)
        integer(sample, 1, id);
      integer(sample, 2, scaled_count);
      integer(sample, 2, scaled_bytes);
      message(profile, 2, sample);
    }
  }
  if (!consumed)
    throw std::invalid_argument("stack without aggregate");
  std::map<std::pair<std::string, std::string>, Number> functions;
  for (auto [pc, id] : locations) {
    std::string location;
    integer(location, 1, id);
    for (std::size_t i = 0; i < mappings.size(); ++i) {
      if (pc >= mappings[i].start && pc < mappings[i].limit) {
        integer(location, 2, i + 1);
        auto &m = mappings[i];
        if (file_path(strings[m.filename]) &&
            pc - m.start <= std::numeric_limits<Number>::max() - m.offset) {
          auto symbol =
              symbolizer.symbol(strings[m.filename], pc - m.start + m.offset);
          if (symbol) {
            auto [it, inserted] =
                functions.try_emplace(std::pair{symbol->name,
                                                symbol->system_name},
                                      functions.size() + 1);
            if (inserted) {
              std::string function;
              integer(function, 1, it->second);
              integer(function, 2, intern(symbol->name));
              integer(function, 3, intern(symbol->system_name));
              message(profile, 5, function);
            }
            std::string frame;
            integer(frame, 1, it->second);
            message(location, 4, frame);
            m.has_functions = true;
          }
        }
        break;
      }
    }
    integer(location, 3, pc);
    message(profile, 4, location);
  }
  for (std::size_t i = 0; i < mappings.size(); ++i) {
    const auto &m = mappings[i];
    std::string encoded;
    integer(encoded, 1, i + 1);
    integer(encoded, 2, m.start);
    integer(encoded, 3, m.limit);
    integer(encoded, 4, m.offset);
    integer(encoded, 5, m.filename);
    if (m.build_id)
      integer(encoded, 6, m.build_id);
    if (m.has_functions)
      integer(encoded, 7, 1);
    message(profile, 3, encoded);
  }
  for (const auto &value : strings)
    message(profile, 6, value);
  integer(profile, 14, 3); // Default sample type: inuse_space.
  return profile;
}
} // namespace hogd
