#include <cstdint>
#include <cstring>
#include <gtest/gtest.h>

#include <algorithm>
#include <memory>
#include <string>

#include "built_contracts/contract_loader.h"
#include "exec_result.hh"
#include "hogd/base/string.h"
#include "hogd/crypto/crypto.h"
#include "hogd/hog_exec/precompiles/context.h"
#include "hogd/hog_exec/runtime/contract_trait.h"
#include "hogd/hog_exec/runtime/native_contract_test_framework.h"
#include "hogd/hog_exec/runtime/runtime.h"
#include "rpc_types.hh"
#include "transaction.hh"
#include "types.hh"

namespace hogd {

// NOLINTNEXTLINE(misc-use-internal-linkage)
class Sha2InContractTest : public ::testing::Test,
                           public NativeContractTestFramework {
public:
  Sha2InContractTest()
    : NativeContractTestFramework(
          false, BlockExecutionMode::kBlockExecutionModeProposal,
          TestDB::kHogDb)
  {}

protected:
  void SetUp() override
  {
    auto contract = hogd::load_rv32_contract("hash_bug.rv32a");
    contract_addr_ = make_contract_address(0);
    write_contract(contract);
    write_deployment(contract_addr_, contract);
    setup_grant_storage_token(contract_addr_, 1'000'000'000);
  }
  ContractAddress contract_addr_;
};

// The hash_bug guest computes sha256("hello") entirely in-contract (real
// codegen, no crypto syscall) and returns the digest through a hand-written
// direct-dispatch entrypoint whose method id is the hex of the historical
// wasm export name "FFFFFFFFFFFFFFFF".
TEST_F(Sha2InContractTest, InvokeFFFFF)
{
  auto hash_expect = decode_hex(
      "2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824");
  std::string hash_input = "hello";
  auto hash_actual = sha256(hash_input);
  ASSERT_EQ(hash_expect.size(), 32);
  ASSERT_EQ(std::memcmp(hash_expect.data(), hash_actual.data(), 32), 0);

  auto method_id_opt = Runtime::method_id_from_name("FFFFFFFFFFFFFFFF");
  ASSERT_TRUE(method_id_opt.has_value())
      << "Failed to parse FFFFFFFFFFFFFFFF as method ID";
  // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
  auto entrypoint = entrypoint_from_method_id(*method_id_opt);

  auto context = make_hog_exec_context();

  auto handler = make_new_system_call_handler(
      SignedTransaction{
          .tx = Transaction{.invocation =
                                Invocation{.invoked_address = contract_addr_,
                                           .entrypoint = entrypoint,
                                           .gas_limit = UINT64_MAX,
                                           .calldata = {}},
                            .uniquifier = make_new_uniquifier()}},
      *context);

  auto res = handler.root_invoke();
  ASSERT_TRUE(res.is_root_success());

  const auto output = handler.get_most_recent_frame_output();
  ASSERT_EQ(output.status_code(), InvocationFrameStatusCode::Return);
  EXPECT_TRUE(std::ranges::equal(output.return_buffer(), hash_actual));

  auto commit_res = handler.try_commit_transaction();
  ASSERT_EQ(commit_res, ExecResult::ExecResultOK);
}

} // namespace hogd
