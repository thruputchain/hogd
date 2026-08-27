#include <gtest/gtest.h>

#include <unordered_map>
#include <utility>

#include "contract.hh"
#include "ffi_enums.hh"
#include "hogd/hog_exec/runtime/item/deployed_contract.h"
#include "hogd/hog_exec/runtime/multiplexed_context.h"
#include "hogd/hog_exec/runtime/runtime.h"
#include "hogd/hog_exec/test_main.h"
#include "types.hh"

namespace hogd {

// verify_contract rejects unknown types at deploy time, so reaching
// make_new_runtime with one is an internal inconsistency, not a contract
// fault: Unrecoverable.
TEST(MultiplexedContextTest, UnregisteredTypeAtInvokeIsUnrecoverable)
{
  MultiplexedContext context(
      std::unordered_map<ContractType,
                         std::pair<RuntimeContext const *, bool>>{});

  DeployedContract deployed(DeployedContractHash{
      .hash = SHA256Hash{},
      .contract_size = 0,
      .contract_type = ContractType::kMockTest,
  });
  auto runtime = context.make_new_runtime(deployed, nullptr);
  ASSERT_FALSE(runtime.has_value());
  EXPECT_EQ(runtime.error().code(), SystemErrorCode::Unrecoverable);
}

TEST(MultiplexedContextTest, UnregisteredTypeVerifyIsDeterministic)
{
  MultiplexedContext context(
      std::unordered_map<ContractType,
                         std::pair<RuntimeContext const *, bool>>{});

  EXPECT_EQ(context.verify_contract(Contract(ContractType::kMockTest)),
            InvokeErrorCode::Deterministic);
}

} // namespace hogd

int
main(int argc, char *argv[]) // NOLINT(bugprone-exception-escape)
{
  return hogd::run_hog_exec_tests(argc, argv);
}
