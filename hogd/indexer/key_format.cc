#include "hogd/indexer/key_format.h"
#include "types.hh"

#include <bit>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

namespace hogd {

void
IndexerKeyFormat::make_tx_key(std::string &key, BlockHeight block_height,
                              SHA256Hash const &tx_hash)
{
  key.clear();
  key.reserve(kTxKeySize);
  // Append block_height as big-endian 8 bytes
  uint64_t height_be = std::byteswap(static_cast<uint64_t>(block_height));
  key.append(reinterpret_cast<const char *>(&height_be), 8);
  // Append tx_hash
  key.append(reinterpret_cast<const char *>(tx_hash.data()), 32);
}

void
IndexerKeyFormat::make_contract_key_prefix(std::string &key,
                                           ContractAddress const &address,
                                           BlockHeight block_height)
{
  key.clear();
  key.reserve(kContractKeyPrefixSize);
  key.append(reinterpret_cast<const char *>(address.data()), 32);
  // Append block_height as big-endian 8 bytes
  uint64_t height_be = std::byteswap(static_cast<uint64_t>(block_height));
  key.append(reinterpret_cast<const char *>(&height_be), 8);
}

void
IndexerKeyFormat::make_contract_key(std::string &key,
                                    ContractAddress const &address,
                                    BlockHeight block_height,
                                    SHA256Hash const &tx_hash,
                                    uint32_t sequence_number)
{
  key.clear();
  key.reserve(kContractKeySize);
  key.append(reinterpret_cast<const char *>(address.data()), 32);
  // Append block_height as big-endian 8 bytes
  uint64_t height_be = std::byteswap(static_cast<uint64_t>(block_height));
  key.append(reinterpret_cast<const char *>(&height_be), 8);
  // Append tx_hash
  key.append(reinterpret_cast<const char *>(tx_hash.data()), 32);
  // Append sequence_number as big-endian 4 bytes
  uint32_t seq_be = std::byteswap(sequence_number);
  key.append(reinterpret_cast<const char *>(&seq_be), 4);
}

bool
IndexerKeyFormat::parse_contract_key(std::string_view key,
                                     ContractAddress *address,
                                     BlockHeight *block_height,
                                     SHA256Hash *tx_hash,
                                     uint32_t *sequence_number)
{
  if (key.size() != kContractKeySize) {
    return false;
  }

  size_t offset = 0;
  constexpr size_t kAddressLen = 32;
  constexpr size_t kHeightLen = 8;
  constexpr size_t kHashLen = 32;
  constexpr size_t kSeqLen = 4;

  if (address != nullptr) {
    std::memcpy(address->data(), key.data() + offset, kAddressLen);
  }
  offset += kAddressLen;

  uint64_t height_be;
  std::memcpy(&height_be, key.data() + offset, kHeightLen);
  *block_height = std::byteswap(height_be);
  offset += kHeightLen;

  std::memcpy(tx_hash->data(), key.data() + offset, kHashLen);
  offset += kHashLen;

  uint32_t seq_be;
  std::memcpy(&seq_be, key.data() + offset, kSeqLen);
  *sequence_number = std::byteswap(seq_be);

  return true;
}

} // namespace hogd
