#include "hogd/hogexec/runtime/contract_trait.h"

#include <cassert>
#include <cstring>
#include <regex>
#include <string_view>
#include <type_traits>

#include "contract.hh"
#include "hogd/base/types.h"
#include "hogd/crypto/crypto.h"
#include "hogd/hogexec/runtime/native_runtime.h"
#include "types.hh"

namespace hogd {

MethodID
sdk_method_id(std::string_view identifier_name,
              std::string_view identifier_version, uint16_t method_id)
{
  assert(std::regex_match(identifier_version.begin(), identifier_version.end(),
                          std::regex(R"(\d+\.\d+\.\d+)")));
  ContractInterfaceIDPreimage preimage{.name = {identifier_name.begin(),
                                                identifier_name.end()},
                                       .vers = {identifier_version.begin(),
                                                identifier_version.end()}};

  auto h = sha256(preimage);

  ContractEntrypointID out{};
  std::memcpy(out.contract_interface_id.data(), h.data(),
              sizeof(out.contract_interface_id));
  std::memcpy(out.contract_method_id.data(), &method_id,
              sizeof(out.contract_method_id));
  static_assert(sizeof(out.contract_method_id) == sizeof(method_id));

  return sdk_method_id(out);
}

MethodID
sdk_method_id(ContractEntrypointID entry_point_id)
{
  static_assert(sizeof(ContractEntrypointID) == sizeof(MethodID));
  static_assert(std::is_trivially_copyable_v<ContractEntrypointID>);
  static_assert(std::is_trivially_copyable_v<MethodID>);

  MethodID out;
  std::memcpy(&out, &entry_point_id, sizeof(MethodID));
  return out;
}

} // namespace hogd
