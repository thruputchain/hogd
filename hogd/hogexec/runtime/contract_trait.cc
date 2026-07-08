#include "hogd/hogexec/runtime/contract_trait.h"

#include <bit>
#include <cassert>
#include <cstring>
#include <regex> // NOLINT(misc-include-cleaner)
#include <string>
#include <string_view>

#include "contract.hh"
#include "hogd/base/types.h"
#include "hogd/crypto/crypto.h"
#include "hogd/hogexec/runtime/runtime.h"
#include "x23/debug.h"

namespace hogd {

namespace {
struct SemverComponents {
  std::string major;
  std::string minor;
  std::string patch;
  // The full, original version string. For a 0.0.0 version the major-version is
  // the entire string (including any -prerelease/+build), matching the SDK.
  std::string full;
};

SemverComponents
parse_semver(std::string_view identifier_version)
{
  // Semver format: MAJOR.MINOR.PATCH[-PRERELEASE][+BUILD]
  // We parse major, minor, patch, and prerelease (ignoring build metadata)
  std::regex semver_regex(
      R"((\d+)\.(\d+)\.(\d+)(-[a-zA-Z0-9.-]+)?(\+[a-zA-Z0-9.-]+)?)");
  std::string version_str(identifier_version);
  std::smatch matches;

  [[maybe_unused]]
  auto ok = std::regex_match(version_str, matches, semver_regex);
  x23assert(ok);
  x23assert(matches.size() >= 4);

  SemverComponents components;
  components.major = matches[1].str();
  components.minor = matches[2].str();
  components.patch = matches[3].str();
  components.full = version_str;

  return components;
}

std::string
major_version(SemverComponents const &components)
{
  if (components.major != "0") {
    return components.major;
  }
  if (components.minor != "0") {
    return "0." + components.minor;
  }
  if (components.patch != "0") {
    return "0.0." + components.patch;
  }
  // 0.0.0[-prerelease][+build]: the SDK (contract-util::major_version) returns
  // the full original version string here, so match it byte-for-byte rather
  // than reconstructing "0.0.0" + a hyphen-stripped prerelease. Otherwise the
  // host derivation diverges from the SDK/generate_header.py kEntrypoints for
  // any 0.0.0-prerelease contract.
  return components.full;
}
} // namespace

ContractEntrypointID
sdk_entrypoint_derivation(std::string_view identifier_name,
                          std::string_view identifier_version,
                          std::string_view method_name)
{
  auto semver_components = parse_semver(identifier_version);

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
  method_name_preimage.append(major_version(semver_components));
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
sdk_entrypoint_derivation(std::u8string_view identifier_name,
                         std::u8string_view identifier_version,
                         std::u8string_view method_name)
{
  // Reinterpret the UTF-8 code units as narrow bytes (byte-identical) and reuse
  // the narrow derivation. Because the source literals are u8"", these bytes are
  // exactly the UTF-8 preimage the SDK/generate_header.py hashed, independent of
  // -fexec-charset; the narrow implementation's separator is likewise u8"::".
  auto as_bytes = [](std::u8string_view s) {
    return std::string_view(reinterpret_cast<const char *>(s.data()), s.size());
  };
  return sdk_entrypoint_derivation(as_bytes(identifier_name),
                                   as_bytes(identifier_version),
                                   as_bytes(method_name));
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
