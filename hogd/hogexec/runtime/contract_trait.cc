#include "hogd/hogexec/runtime/contract_trait.h"

#include <bit>
#include <cstring>
#include <string>
#include <string_view>
#include <utility>

#include "contract.hh"
#include "hogd/base/types.h"
#include "hogd/crypto/crypto.h"
#include "hogd/hogexec/runtime/runtime.h"
#include "x23/debug.h"

namespace hogd {

namespace {
// Returns the SDK/spec "major version" fold of a semver string, matching
// sdk_rs contract-util::major_version and built_contracts/generate_header.py:
//
//   MAJOR.MINOR.PATCH[-prerelease][+build]:
//     major > 0                    -> "MAJOR"
//     major == 0, minor > 0        -> "MAJOR.MINOR"
//     major == minor == 0, patch>0 -> "MAJOR.MINOR.PATCH"
//     all three zero               -> the full original string (prerelease and
//                                      build metadata included)
//
// The result is always a prefix slice of the input, and parsing keys only off
// raw byte values (ASCII/UTF-8 digits 0x30..0x39 and '.' == 0x2e). It therefore
// introduces no ordinary-literal bytes and folds identically regardless of the
// translation unit's ordinary execution charset (`-fexec-charset`). An earlier
// implementation compared against ordinary `"0"` literals and matched an
// ordinary-literal `std::regex`, so under a non-UTF-8 charset it misfolded (or
// aborted on) UTF-8 preimage bytes. See hogd issue #2898.
std::string
major_version(std::string_view version)
{
  constexpr unsigned char kDot = 0x2e; // '.'

  auto is_digit = [](unsigned char b) { return b >= 0x30 && b <= 0x39; };

  // Consume a run of digits from `pos`; return {run_is_nonzero, end_pos}.
  auto scan_number = [&](size_t pos) -> std::pair<bool, size_t> {
    size_t start = pos;
    bool nonzero = false;
    while (pos < version.size() &&
           is_digit(static_cast<unsigned char>(version[pos]))) {
      if (static_cast<unsigned char>(version[pos]) != 0x30) {
        nonzero = true;
      }
      ++pos;
    }
    x23assert(pos > start); // at least one digit
    return {nonzero, pos};
  };

  auto [major_nonzero, after_major] = scan_number(0);
  if (major_nonzero) {
    return std::string(version.substr(0, after_major));
  }
  x23assert(after_major < version.size() &&
            static_cast<unsigned char>(version[after_major]) == kDot);

  auto [minor_nonzero, after_minor] = scan_number(after_major + 1);
  if (minor_nonzero) {
    return std::string(version.substr(0, after_minor));
  }
  x23assert(after_minor < version.size() &&
            static_cast<unsigned char>(version[after_minor]) == kDot);

  auto [patch_nonzero, after_patch] = scan_number(after_minor + 1);
  if (patch_nonzero) {
    return std::string(version.substr(0, after_patch));
  }

  return std::string(version);
}
} // namespace

ContractEntrypointID
sdk_entrypoint_derivation(std::string_view identifier_name,
                          std::string_view identifier_version,
                          std::string_view method_name)
{
  // Components are joined with a "::" domain separator so that distinct
  // (name, version, method) triples cannot collide on the same preimage.
  //
  // The separator is a `u8"..."` literal so its bytes are the UTF-8 encoding
  // (0x3a 0x3a) mandated by the standard, independent of the ordinary execution
  // character set (`-fexec-charset`). An ordinary `"::"` literal would hash
  // implementation-defined code units, so two honest validators built with
  // different execution charsets could derive divergent ids. See hogd issue
  // #2573.
  constexpr std::u8string_view kSeparator = u8"::";
  const auto *sep = reinterpret_cast<const char *>(kSeparator.data());

  std::string method_name_preimage;
  method_name_preimage.append(identifier_name);
  method_name_preimage.append(sep, kSeparator.size());
  method_name_preimage.append(major_version(identifier_version));
  method_name_preimage.append(sep, kSeparator.size());
  method_name_preimage.append(method_name);

  auto h = sha256(method_name_preimage);

  ContractEntrypointID out{};
  auto *out_ptr = reinterpret_cast<uint8_t *>(&out);
  std::memcpy(out_ptr, h.data(), 8);
  static_assert(sizeof(out) == 8);
  return out;
}

ContractEntrypointID
entrypoint_id_from_utf8_preimage(std::u8string_view preimage)
{
  // Hash an already-folded UTF-8 preimage (`interface::major_version::method`).
  // The `u8""` bytes are UTF-8 regardless of `-fexec-charset`, so
  // reinterpreting them to narrow bytes is byte-identical to what the
  // SDK/generate_header.py hashed. See hogd issue #2898.
  std::string bytes(reinterpret_cast<const char *>(preimage.data()),
                    preimage.size());
  auto h = sha256(bytes);

  ContractEntrypointID out{};
  std::memcpy(reinterpret_cast<uint8_t *>(&out), h.data(), 8);
  static_assert(sizeof(out) == 8);
  return out;
}

Runtime::MethodID
sdk_method_id(ContractEntrypointID entry_point_id)
{
  Runtime::MethodID out{};
  std::memcpy(reinterpret_cast<uint8_t *>(&out), entry_point_id.data(),
              sizeof(entry_point_id));
  static_assert(sizeof(out) == sizeof(entry_point_id));
  return std::byteswap(out);
}

// inverse of sdk_method_id
ContractEntrypointID
entrypoint_from_method_id(Runtime::MethodID method_id)
{
  ContractEntrypointID out{};
  uint64_t byteswapped = std::byteswap(method_id);
  std::memcpy(reinterpret_cast<uint8_t *>(&out), &byteswapped,
              sizeof(byteswapped));
  static_assert(sizeof(out) == sizeof(method_id));
  return out;
}

} // namespace hogd
