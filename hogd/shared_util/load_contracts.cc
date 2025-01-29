#include "hogd/shared_util/load_contracts.h"

#include <cstdio>
#include <cstring>
#include <string>

#include "contract.hh"
#include "hogd/crypto/crypto.h"
#include "types.hh"

namespace hogd {

std::pair<SHA256Hash, std::shared_ptr<const Contract>>
load_built_rs_contract(std::string_view filename)
{
  std::string file =
      std::string(CMAKE_ROOT_BINARY_DIR) +
      std::string(
          "/hogd/rs_contracts/built_wasms/wasm32-unknown-unknown/release/") +
      std::string(filename);
  FILE *f = std::fopen(file.c_str(), "r");
  if (f == nullptr)
    throw std::runtime_error("failed to load built rs wasm contract: " + file);

  auto contract = std::make_shared<Contract>();
  auto &contents = contract->contract_data;
  constexpr size_t kBufferSize = 65536;
  char buf[kBufferSize];

  int count = -1;
  while (count != 0) {
    count = std::fread(buf, sizeof(char), kBufferSize, f);
    std::span<char> buf_span(&buf[0], count);
    if (count > 0)
      contents.append_range(buf_span);
  }
  std::fclose(f);

  auto serialized = x23::serialize(*contract);
  SHA256Hash hash = sha256(std::as_bytes(std::span(serialized)));

  return {hash, contract};
}

MethodID
compute_sdk_name(std::string_view name)
{
  SHA256Hash hash = sha256(name);
  return std::byteswap(*reinterpret_cast<uint32_t *>(hash.data()));
}

SignedTransaction
genesis_deployer_deploy_and_invoke(
    SHA256Hash const &nonce, SHA256Hash const &contract_hash,
    std::shared_ptr<const Contract> const &contract_to_create, MethodID method,
    Calldata const &calldata)
{
  SignedTransaction stx;
  stx.tx.invocation.invoked_address = kDeployerAddress;
  stx.tx.invocation.method = compute_sdk_name("deploy_and_invoke");
  stx.tx.invocation.gas_limit = UINT64_MAX;
  stx.tx.invocation.calldata =
      make_calldata(nonce, contract_hash, method, calldata);

  if (contract_to_create) {
    stx.tx.contracts_to_deploy.push_back(*contract_to_create);
  }
  return stx;
}

} // namespace hogd
