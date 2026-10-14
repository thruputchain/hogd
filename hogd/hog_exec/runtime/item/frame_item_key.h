#pragma once

#include <array>
#include <cstring>
#include <span>

#include "hogd/base/types.h"
#include "hogd/crypto/crypto.h"
#include "types.hh"

namespace hogd {

// A storage key as the invocation frames index it: address, key type and key
// payload laid out contiguously, plus the SipHash of those bytes, computed once
// when the key enters the frame stack.
//
// AddressAndKey is the XDR type. Hashing it means serializing it into a buffer
// first (std::hash<AddressAndKey>), and comparing it means comparing a
// discriminated union. A first touch probes every open frame with the same
// key, so with the XDR type as the map key every probe re-serializes and
// re-hashes 68 bytes -- measured at ~50 ns per open parent frame, on every
// first touch. Here the bytes are laid out and hashed once per syscall, each
// probe compares the cached hash and then the bytes, and the XDR form is
// rebuilt only where a delta is emitted.
//
// The hash is SipHash24 under the process-wide random key, as
// std::hash<AddressAndKey> uses: contract-data keys are chosen by contracts,
// so the frame map's hash has to be keyed. Nothing about the bytes' layout is
// consensus-relevant; it is an in-memory index.
class FrameItemKey {
public:
  static constexpr size_t kAddressBytes = sizeof(ContractAddress);
  static constexpr size_t kTypeBytes = sizeof(int32_t);
  static constexpr size_t kPayloadBytes = sizeof(StorageKeyContractData);
  static constexpr size_t kBytes = kAddressBytes + kTypeBytes + kPayloadBytes;
  static_assert(kAddressBytes == 32 && kPayloadBytes == 32);
  using Bytes = std::array<uint8_t, kBytes>;

  static FrameItemKey from(AddressAndKey const &addrkey)
  {
    const auto type = addrkey.key.key_type();
    if (type == StorageKeyType::ContractData) {
      return lay_out(addrkey.address, type, &addrkey.key.contract_data_key());
    }
    return lay_out(addrkey.address, type, nullptr);
  }

  // The contract-data key under `address`, without building the XDR union on
  // the way: this is the shape every storage syscall a contract makes takes.
  static FrameItemKey contract_data(ContractAddress const &address,
                                    StorageKeyContractData const &key)
  {
    return lay_out(address, StorageKeyType::ContractData, &key);
  }

  // The XDR form, for a delta or a lookup in a structure keyed by it.
  AddressAndKey to_address_and_key() const
  {
    int32_t type_value = 0;
    std::memcpy(&type_value, bytes_.data() + kAddressBytes, kTypeBytes);
    const auto type = static_cast<StorageKeyType>(type_value);
    StorageKey key(type);
    if (type == StorageKeyType::ContractData) {
      std::memcpy(key.contract_data_key().data(),
                  bytes_.data() + kAddressBytes + kTypeBytes, kPayloadBytes);
    }
    AddressAndKey out{};
    std::memcpy(out.address.data(), bytes_.data(), kAddressBytes);
    out.key = key;
    return out;
  }

  size_t hash() const noexcept { return hash_; }
  // The laid-out bytes, for a container that stores keys by their parts
  // (FlatKeySet).
  Bytes const &bytes() const noexcept { return bytes_; }

  bool operator==(FrameItemKey const &other) const noexcept
  {
    return hash_ == other.hash_ && bytes_ == other.bytes_;
  }

private:
  explicit FrameItemKey(Bytes const &bytes)
    : bytes_(bytes),
      hash_(SipHash24::get_default()(std::as_bytes(std::span(bytes_))))
  {}

  static FrameItemKey lay_out(ContractAddress const &address,
                              StorageKeyType type,
                              StorageKeyContractData const *payload)
  {
    Bytes bytes{};
    std::memcpy(bytes.data(), address.data(), kAddressBytes);
    const auto type_value = static_cast<int32_t>(type);
    std::memcpy(bytes.data() + kAddressBytes, &type_value, kTypeBytes);
    if (payload != nullptr) {
      std::memcpy(bytes.data() + kAddressBytes + kTypeBytes, payload->data(),
                  kPayloadBytes);
    }
    return FrameItemKey(bytes);
  }

  Bytes bytes_;
  size_t hash_;
};

struct FrameItemKeyHash {
  size_t operator()(FrameItemKey const &key) const noexcept
  {
    return key.hash();
  }
};

} // namespace hogd
