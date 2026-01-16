#include "hogd/hogexec/runtime/contract_trait.h"

#include <bit>
#include <cassert>
#include <cstring>
#include <regex> // NOLINT(misc-include-cleaner)
#include <string_view>

#include "contract.hh"
#include "hogd/base/types.h"
#include "hogd/crypto/crypto.h"
#include "hogd/hogexec/runtime/runtime.h"

namespace hogd {

ContractEntrypointID
sdk_entrypoint_derivation(std::string_view identifier_name,
                          std::string_view identifier_version,
                          uint16_t method_id)
{
  assert(std::regex_match(identifier_version.begin(), identifier_version.end(),
                          std::regex(R"(\d+\.\d+\.\d+)")));
  ContractInterfaceIDPreimage preimage{.name = {identifier_name.begin(),
                                                identifier_name.end()},
                                       .vers = {identifier_version.begin(),
                                                identifier_version.end()}};

  auto h = sha256(preimage);

  uint16_t method_id_byteswapped = std::byteswap(method_id);

  ContractEntrypointID out{};
  auto *out_ptr = reinterpret_cast<uint8_t *>(&out);
  std::memcpy(out_ptr, h.data(), 6);
  std::memcpy(out_ptr + 6, &method_id_byteswapped, 2);
  static_assert(sizeof(out) == 6 + 2);
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
