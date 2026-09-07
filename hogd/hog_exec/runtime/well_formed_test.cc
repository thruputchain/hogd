#include <gtest/gtest.h>

#include "contract.hh"
#include "hogd/hog_exec/runtime/well_formed.h"
#include "transaction.hh"

namespace hogd {

namespace {

// The predicate reads only the contract type, so these fixtures set nothing
// else: no blob, no native contract id. Anything more would be state it never
// looks at.
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

// rv32 is the only contract type a transaction may attach.
TEST(WellFormedTest, Rv32ContractIsAttachable)
{
  EXPECT_TRUE(HogexecAdmission::is_contract_attachable(make_rv32_contract()));
}

// The condition this predicate exists for: the banned-redeploy check refuses
// every native id, so an attached native could never be deployed.
TEST(WellFormedTest, NativeContractIsNotAttachable)
{
  EXPECT_FALSE(
      HogexecAdmission::is_contract_attachable(make_native_contract()));
}

// The common case: nothing attached at all.
TEST(WellFormedTest, TransactionWithNoAttachmentsIsWellFormed)
{
  Transaction tx{};
  EXPECT_TRUE(HogexecAdmission::is_tx_well_formed(tx));
}

TEST(WellFormedTest, Rv32AttachmentsAreWellFormed)
{
  Transaction tx{};
  tx.contracts_to_deploy.push_back(make_rv32_contract());
  tx.contracts_to_deploy.push_back(make_rv32_contract());
  EXPECT_TRUE(HogexecAdmission::is_tx_well_formed(tx));
}

TEST(WellFormedTest, NativeAttachmentIsNotWellFormed)
{
  Transaction tx{};
  tx.contracts_to_deploy.push_back(make_native_contract());
  EXPECT_FALSE(HogexecAdmission::is_tx_well_formed(tx));
}

// One bad attachment condemns the transaction, wherever it sits in the list.
TEST(WellFormedTest, NativeAttachmentAmongRv32OnesIsNotWellFormed)
{
  Transaction tx{};
  tx.contracts_to_deploy.push_back(make_rv32_contract());
  tx.contracts_to_deploy.push_back(make_native_contract());
  tx.contracts_to_deploy.push_back(make_rv32_contract());
  EXPECT_FALSE(HogexecAdmission::is_tx_well_formed(tx));
}

} // namespace hogd
