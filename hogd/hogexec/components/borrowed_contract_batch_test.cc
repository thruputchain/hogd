#include <gtest/gtest.h>

#include <initializer_list>
#include <memory>
#include <unordered_map>
#include <unordered_set>

#include "contract.hh"
#include "executor_to_db_rpc.hh"
#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h" // IWYU pragma: keep
#include "hogd/crypto/crypto.h"
#include "hogd/hogexec/components/borrowed_contract_batch.h"
#include "types.hh"
#include "x23/serialize.h"
#include "x23/x23.h"

namespace hogd {
namespace {

Contract
make_wasm_contract(std::initializer_list<uint8_t> bytes)
{
  Contract contract(ContractType::kWasmi);
  contract.wasmi_contract().wasm = bytes;
  return contract;
}

NewContractBatch
materialize_owned_batch(BorrowedContractBatch::hash_set_t const &hashes,
                        BorrowedContractBatch::contract_map_t const &contracts)
{
  NewContractBatch out;
  for (auto const &hash : hashes) {
    if (auto it = contracts.find(hash); it != contracts.end()) {
      out.push_back(*it->second);
    }
  }
  return out;
}

} // namespace

static_assert(x23::serializes_like<BorrowedContractBatch, NewContractBatch>);

TEST(BorrowedContractBatchTest, SerializesLikeOwnedBatch)
{
  Contract c1 = make_wasm_contract({0x01, 0x02, 0x03});
  Contract c2 = make_wasm_contract({0xAA, 0xBB});
  auto c1_hash = sha256(c1);
  auto c2_hash = sha256(c2);

  std::unordered_set<SHA256Hash> hashes{c1_hash, c2_hash};
  std::unordered_map<SHA256Hash, std::unique_ptr<const Contract>> contracts;
  contracts.emplace(c1_hash, std::make_unique<const Contract>(c1));
  contracts.emplace(c2_hash, std::make_unique<const Contract>(c2));

  BorrowedContractBatch borrowed(hashes, contracts);

  NewContractBatch owned = materialize_owned_batch(hashes, contracts);
  EXPECT_EQ(x23::serialize(borrowed), x23::serialize(owned));
}

TEST(BorrowedContractBatchTest, SerializesEmptyBatch)
{
  std::unordered_set<SHA256Hash> hashes;
  std::unordered_map<SHA256Hash, std::unique_ptr<const Contract>> contracts;
  BorrowedContractBatch borrowed(hashes, contracts);

  NewContractBatch owned;
  EXPECT_EQ(x23::serialize(borrowed), x23::serialize(owned));
}

TEST(BorrowedContractBatchTest, SkipsMissingHashes)
{
  Contract c1 = make_wasm_contract({0x01, 0x02, 0x03});
  auto c1_hash = sha256(c1);

  Contract missing_contract = make_wasm_contract({0x10, 0x20});
  auto missing_hash = sha256(missing_contract);

  std::unordered_set<SHA256Hash> hashes{c1_hash, missing_hash};
  std::unordered_map<SHA256Hash, std::unique_ptr<const Contract>> contracts;
  contracts.emplace(c1_hash, std::make_unique<const Contract>(c1));

  BorrowedContractBatch borrowed(hashes, contracts);

  NewContractBatch owned = materialize_owned_batch(hashes, contracts);
  EXPECT_EQ(x23::serialize(borrowed), x23::serialize(owned));
}

} // namespace hogd
