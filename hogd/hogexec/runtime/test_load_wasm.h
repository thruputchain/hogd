#pragma once

#include <cstdint>
#include <cstdio>
#include <string>
#include <type_traits>
#include <vector>

#include "contract.hh"
#include "hogd/crypto/crypto.h"
#include "transaction.hh"
#include "types.hh"
#include "x23/serialize.h"
#include "x23/vec.h"

namespace hogd ::test {

[[maybe_unused]]
static ContractBlob
load_wasm_from_file(std::string_view filename)
{
  std::string file = std::string(CMAKE_ROOT_BINARY_DIR) +
                     std::string("/hogd/hogexec/runtime/") +
                     std::string(filename);
  FILE *f = std::fopen(file.c_str(), "r");
  if (f == nullptr)
    throw std::runtime_error("failed to load wasm file: " + file);

  ContractBlob contents;
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

  return contents;
}

[[maybe_unused]]
static std::pair<SHA256Hash, std::shared_ptr<const Contract>>
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

[[maybe_unused]] Calldata static make_calldata()
{
  return {};
}

template<typename... Cs>
Calldata static make_calldata(std::vector<uint8_t> const &data,
                              Cs const &...additional)
{
  x23::Vec<uint8_t> out;

  out.resize_uninitialized(data.size());
  std::memcpy(out.data(), data.data(), data.size());

  auto rest = make_calldata(additional...);

  out += rest;
  return out;
}

template<typename C, typename... Cs> requires std::is_trivially_copyable_v<C>
Calldata static make_calldata(C const &data, Cs const &...additional)
{
  const auto *addr = reinterpret_cast<const uint8_t *>(&data);
  x23::Vec<uint8_t> out;

  if constexpr (!std::is_empty_v<C>) {
    out.resize_uninitialized(sizeof(C));
    std::memcpy(out.data(), addr, sizeof(C));
  }

  auto rest = make_calldata(additional...);

  out += rest;
  return out;
}

[[maybe_unused]]
MethodSpecifier static get_sdk_name(std::string const &v)
{
  SHA256Hash hash = sha256(std::as_bytes(std::span(v)));

  MethodID out_name = std::byteswap(*reinterpret_cast<uint32_t *>(hash.data()));

  MethodSpecifier specifier(MethodSpecifierType::kDefaultUint32);
  specifier.method_id_uint32() = out_name;

  return specifier;
}

[[maybe_unused]]
SHA256Hash static make_nonce(uint64_t id)
{
  return sha256(std::as_bytes(std::span(reinterpret_cast<uint8_t *>(&id), 8)));
}

} // namespace hogd::test
