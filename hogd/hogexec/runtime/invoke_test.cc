#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <span>
#include <vector>

#include "hogd/base/types.h"
#include "hogd/crypto/crypto.h"
#include "hogd/hogexec/runtime/storage_snapshot_cache.h"
#include "hogd/hogexec/runtime/syscall_handler.h"
#include "hogd/hogexec/runtime/test_load_wasm.h"
#include "hogd/hogexec/runtime/runtime_test_framework.h"
#include "transaction.hh"
#include "types.hh"
#include "x23/vec.h"

namespace hogd {

class InvokeTest : public ::testing::Test , public test::RuntimeTestFramework {
protected:
  void SetUp() override
  {
    test::RuntimeTestFramework::initialize();

    auto [h1, c1] = test::load_built_rs_contract("test_invoke_1.wasm");
    auto [h2, c2] = test::load_built_rs_contract("test_invoke_2.wasm");

    std::vector<uint8_t> bytes1 = {0, 1, 2, 3};
    std::vector<uint8_t> bytes2 = {3, 2, 1, 0};
    addr1_ = sha256(std::as_bytes(std::span(bytes1)));
    addr2_ = sha256(std::as_bytes(std::span(bytes2)));

    contract_map_->add_contract(h1, c1);
    contract_map_->deploy_contract(addr1_, h1);

    contract_map_->add_contract(h2, c2);
    contract_map_->deploy_contract(addr2_, h2);
  }

  ContractAddress addr1_;
  ContractAddress addr2_;
};

TEST_F(InvokeTest, CalldataPassthroughSuccess)
{
  SignedTransaction stx;
  stx.tx.invocation.invoked_address = addr2_;
  stx.tx.invocation.method_name = 0;
  stx.tx.invocation.gas_limit = UINT64_MAX;
  stx.tx.invocation.calldata =
      test::make_calldata(addr1_, static_cast<MethodID>(0),
                          std::vector<uint8_t>{0, 1, 2, 3});

  SystemCallHandler handler(stx, *snapshot_cache_);

  auto res = handler.root_invoke();

  EXPECT_TRUE(!!res.result);

  auto const &logs = handler.get_logs();

  ASSERT_EQ(logs.size(), 2);

  EXPECT_EQ(logs[0].source_address, addr1_);
  check_log_eq(logs[0], test::make_calldata(addr2_));

  EXPECT_EQ(logs[1].source_address, addr1_);
  check_log_eq(logs[1], test::make_calldata(std::vector<uint8_t>{0, 1, 2, 3}));
}

TEST_F(InvokeTest, CalldataPassthroughFail)
{
  SignedTransaction stx;
  stx.tx.invocation.invoked_address = addr2_;
  stx.tx.invocation.method_name = 0;
  stx.tx.invocation.gas_limit = UINT64_MAX;
  stx.tx.invocation.calldata =
      test::make_calldata(addr1_, static_cast<MethodID>(1),
                          std::vector<uint8_t>{0, 1, 2, 3});

  SystemCallHandler handler(stx, *snapshot_cache_);

  auto res = handler.root_invoke();

  EXPECT_FALSE(!!res.result);

  auto const &logs = handler.get_logs();

  ASSERT_EQ(logs.size(), 0);
}

} // namespace hogd
