#include <gtest/gtest.h>

#include <unordered_map>
#include <utility>

#include "contract.hh"
#include "hogd/hog_exec/runtime/item/deployed_contract.h"
#include "hogd/hog_exec/runtime/multiplexed_context.h"
#include "hogd/hog_exec/runtime/runtime.h"
#include "hogd/hog_exec/test_main.h"

namespace hogd {

TEST(RuntimeTest, MethodIDRoundTrip)
{
  auto check_round_trip = [](Runtime::MethodID id) {
    auto name = Runtime::invoke_name(id);
    auto res = Runtime::method_id_from_name(name);
    ASSERT_EQ(res, id);
  };

  check_round_trip(0x1234'5678'90AB'CDEF);
  check_round_trip(0);
}

// NOLINTBEGIN(bugprone-unchecked-optional-access)
TEST(RuntimeTest, MethodIDFormat)
{
  auto id = Runtime::method_id_from_name("0000000000000000");
  ASSERT_TRUE(id.has_value());
  ASSERT_EQ(id, 0);

  ASSERT_EQ(Runtime::invoke_name(0), "0000000000000000");
  ASSERT_EQ(Runtime::invoke_name(0x0000'0678'90AB'CDEF), "0000067890ABCDEF");
}
// NOLINTEND(bugprone-unchecked-optional-access)

// A routing miss returns a null runtime through a successful status; the
// caller (SystemCallHandler::invoke) turns that into a deterministic syscall
// rejection with zero gas.
TEST(MultiplexedContextTest, UnregisteredTypeYieldsNullRuntime)
{
  MultiplexedContext context(
      std::unordered_map<ContractType, std::pair<RuntimeContext *, bool>>{});

  DeployedContract deployed(DeployedContractHash{
      .hash = SHA256Hash{},
      .contract_size = 0,
      .contract_type = ContractType::kMockTest,
  });
  auto runtime = context.make_new_runtime(deployed, nullptr);
  ASSERT_TRUE(runtime.has_value());
  EXPECT_EQ(runtime.value(), nullptr);
}

TEST(MultiplexedContextTest, UnregisteredTypeVerifyIsDeterministic)
{
  MultiplexedContext context(
      std::unordered_map<ContractType, std::pair<RuntimeContext *, bool>>{});

  EXPECT_EQ(context.verify_contract(Contract(ContractType::kMockTest)),
            InvokeErrorCode::Deterministic);
}

} // namespace hogd

int
main(int argc, char *argv[]) // NOLINT(bugprone-exception-escape)
{
  return hogd::run_hog_exec_tests(argc, argv);
}
