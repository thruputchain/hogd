#include "hogd/indexer/key_format.h"

#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "types.hh"
#include "x23/serialize.h"

#include <bit>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>

namespace hogd {

std::string
IndexerKeyFormat::make_block_identity_meta_key(BlockHeight block_height)
{
  std::string key(kBlockIdentityMetaKeyPrefix);
  const uint64_t encoded = std::byteswap(static_cast<uint64_t>(block_height));
  key.append(reinterpret_cast<const char *>(&encoded), sizeof(encoded));
  return key;
}

std::string
IndexerKeyFormat::make_epoch_history_meta_key(const ProtocolCommit &protocol)
{
  std::string key(kEpochHistoryMetaKeyPrefix);
  const x23::Buf encoded = x23::serialize(protocol);
  key.append(reinterpret_cast<const char *>(encoded.data()), encoded.size());
  return key;
}

void
IndexerKeyFormat::make_tx_key(std::string &key, SHA256Hash const &tx_hash)
{
  key.clear();
  key.reserve(kTxKeySize);
  // Append tx_hash
  key.append(reinterpret_cast<const char *>(tx_hash.data()), 32);
}

void
IndexerKeyFormat::make_contract_key_prefix(std::string &key,
                                           ContractAddress const &address,
                                           RecordKind kind,
                                           BlockHeight block_height)
{
  key.clear();
  key.reserve(kContractKeyPrefixSize);
  key.append(reinterpret_cast<const char *>(address.data()), 32);
  key.push_back(static_cast<char>(kind));
  uint64_t height_be = std::byteswap(static_cast<uint64_t>(block_height));
  key.append(reinterpret_cast<const char *>(&height_be), 8);
}

void
IndexerKeyFormat::make_contract_key(std::string &key,
                                    ContractAddress const &address,
                                    RecordKind kind, BlockHeight block_height,
                                    SHA256Hash const &tx_hash,
                                    uint32_t sequence_number)
{
  key.clear();
  key.reserve(kContractKeySize);
  key.append(reinterpret_cast<const char *>(address.data()), 32);
  key.push_back(static_cast<char>(kind));
  uint64_t height_be = std::byteswap(static_cast<uint64_t>(block_height));
  key.append(reinterpret_cast<const char *>(&height_be), 8);
  key.append(reinterpret_cast<const char *>(tx_hash.data()), 32);
  uint32_t seq_be = std::byteswap(sequence_number);
  key.append(reinterpret_cast<const char *>(&seq_be), 4);
}

std::optional<IndexerKeyFormat::ParsedContractKey>
IndexerKeyFormat::parse_contract_key(std::string_view key)
{
  if (key.size() != kContractKeySize) {
    return std::nullopt;
  }

  size_t offset = 0;
  constexpr size_t kAddressLen = 32;
  constexpr size_t kKindLen = 1;
  constexpr size_t kHeightLen = 8;
  constexpr size_t kHashLen = 32;
  constexpr size_t kSeqLen = 4;

  ParsedContractKey out;

  std::memcpy(out.address.data(), key.data() + offset, kAddressLen);
  offset += kAddressLen;

  // A wrong-size key (above) is a benign "not a contract key" miss.  But a
  // correctly-sized key whose kind byte is neither kEvent nor kRootTx is a
  // corrupt row (or one written by a newer binary): the data is present and
  // wrong, so fail loud rather than silently reporting it as absent.
  const auto raw_kind = static_cast<uint8_t>(key[offset]);
  CHECK(raw_kind == static_cast<uint8_t>(kEvent) ||
        raw_kind == static_cast<uint8_t>(kRootTx));
  out.kind = static_cast<RecordKind>(raw_kind);
  offset += kKindLen;

  uint64_t height_be;
  std::memcpy(&height_be, key.data() + offset, kHeightLen);
  out.block_height = std::byteswap(height_be);
  offset += kHeightLen;

  std::memcpy(out.tx_hash.data(), key.data() + offset, kHashLen);
  offset += kHashLen;

  uint32_t seq_be;
  std::memcpy(&seq_be, key.data() + offset, kSeqLen);
  out.sequence_number = std::byteswap(seq_be);

  return out;
}

} // namespace hogd
