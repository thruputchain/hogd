#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>

#include "contract.hh"
#include "ffi_enums.hh"
#include "hogd/hog_mempool/well_formed.h"
#include "transaction.hh"

namespace hogd {

namespace {

Contract
make_rv32_contract(uint8_t fill)
{
  Contract contract(ContractType::kRv32);
  contract.rv32_contract().data.resize_uninitialized(16);
  for (size_t i = 0; i < contract.rv32_contract().data.size(); ++i) {
    contract.rv32_contract().data[i] = static_cast<uint8_t>(i + fill);
  }
  return contract;
}

Contract
make_native_contract()
{
  Contract contract(ContractType::kNative);
  contract.native_contract_id() =
      HogexecPrecompiledNativeContracts::kNativeContractDefaultWallet;
  return contract;
}

} // namespace

// The common case: nothing attached at all.
TEST(WellFormedTest, TransactionWithNoAttachmentsIsWellFormed)
{
  SignedTransaction tx{};
  EXPECT_TRUE(is_tx_well_formed(tx));
}

// rv32 is the only contract type a transaction may attach.
TEST(WellFormedTest, Rv32AttachmentsAreWellFormed)
{
  SignedTransaction tx{};
  tx.tx.contracts_to_deploy.push_back(make_rv32_contract(1));
  tx.tx.contracts_to_deploy.push_back(make_rv32_contract(2));
  EXPECT_TRUE(is_tx_well_formed(tx));
}

// The condition this filter exists for: hog_exec refuses an attached native
// contract, so the mempool must not admit one.
TEST(WellFormedTest, NativeAttachmentIsNotWellFormed)
{
  SignedTransaction tx{};
  tx.tx.contracts_to_deploy.push_back(make_native_contract());
  EXPECT_FALSE(is_tx_well_formed(tx));
}

// One bad attachment condemns the transaction, wherever it sits in the list.
TEST(WellFormedTest, NativeAttachmentAmongRv32OnesIsNotWellFormed)
{
  SignedTransaction tx{};
  tx.tx.contracts_to_deploy.push_back(make_rv32_contract(1));
  tx.tx.contracts_to_deploy.push_back(make_native_contract());
  tx.tx.contracts_to_deploy.push_back(make_rv32_contract(2));
  EXPECT_FALSE(is_tx_well_formed(tx));
}

} // namespace hogd
