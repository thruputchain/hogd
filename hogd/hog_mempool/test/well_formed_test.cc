#include <gtest/gtest.h>

#include "contract.hh"
#include "hogd/hog_mempool/well_formed.h"
#include "transaction.hh"

namespace hogd {

namespace {

// is_tx_well_formed() reads only the contract type, so these fixtures set
// nothing else: no blob, no native contract id. Anything more would be state
// the predicate never looks at.
Contract
make_rv32_contract()
{
  return Contract(ContractType::kRv32);
}

Contract
make_native_contract()
{
  return Contract(ContractType::kNative);
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
  tx.tx.contracts_to_deploy.push_back(make_rv32_contract());
  tx.tx.contracts_to_deploy.push_back(make_rv32_contract());
  EXPECT_TRUE(is_tx_well_formed(tx));
}

// The condition this filter exists for: hog_exec refuses an attached native
// contract, so the mempool must not admit one into a block we propose.
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
  tx.tx.contracts_to_deploy.push_back(make_rv32_contract());
  tx.tx.contracts_to_deploy.push_back(make_native_contract());
  tx.tx.contracts_to_deploy.push_back(make_rv32_contract());
  EXPECT_FALSE(is_tx_well_formed(tx));
}

} // namespace hogd
