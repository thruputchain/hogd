#include <gtest/gtest.h>

#include <sys/socket.h>

#include <cstdint>
#include <cstring>

#include "hogd/base/types.h"
#include "hogd/hogexec/runtime/runtime_test_framework.h"
#include "hogd/hogexec/runtime/storage_snapshot_cache.h"
#include "hogd/hogexec/runtime/syscall_handler.h"
#include "hogd/hogexec/runtime/test_load_wasm.h"
#include "hogd/softstate/local_softstate_map.h"
#include "transaction.hh"
#include "types.hh"
#include "x23/rpc.h"
#include "x23/threadpool.h"
#include "x23/vec.h"

namespace hogd {

class CalldataTest : public ::testing::Test, public test::RuntimeTestFramework {
protected:
  void SetUp() override
  {
    test::RuntimeTestFramework::initialize();

    auto [h, c] = test::load_built_rs_contract("test_calldata.wasm");

    contract_map_->add_contract(h, c);
    contract_map_->deploy_contract(addr_, h);
    contract_map_->commit_round();
  }

  ContractAddress addr_ = ContractAddress{};
};

TEST_F(CalldataTest, NoCalldata)
{

  SignedTransaction stx;
  stx.tx.invocation.invoked_address = addr_;
  stx.tx.invocation.method_name = 0;
  stx.tx.invocation.gas_limit = UINT64_MAX;
  stx.nonce = 0;

  SystemCallHandler handler(stx, *snapshot_cache_);

  auto res = handler.root_invoke();

  EXPECT_TRUE(!!res.result);

  auto const &logs = handler.get_logs();

  ASSERT_EQ(logs.size(), 1);

  EXPECT_EQ(logs[0].source_address, addr_);
  check_log_eq(logs[0], stx.tx.invocation.calldata);
}

TEST_F(CalldataTest, SomeCalldata)
{

  SignedTransaction stx;
  stx.tx.invocation.invoked_address = addr_;
  stx.tx.invocation.method_name = 0;
  stx.tx.invocation.gas_limit = UINT64_MAX;
  stx.tx.invocation.calldata = {0, 1, 2, 3, 4, 5};
  stx.nonce = 0;

  SystemCallHandler handler(stx, *snapshot_cache_);

  auto res = handler.root_invoke();

  EXPECT_TRUE(!!res.result);

  auto const &logs = handler.get_logs();

  ASSERT_EQ(logs.size(), 1);

  EXPECT_EQ(logs[0].source_address, addr_);
  check_log_eq(logs[0], stx.tx.invocation.calldata);
}

TEST_F(CalldataTest, TooMuchCalldata)
{
  SignedTransaction stx;
  stx.tx.invocation.invoked_address = addr_;
  stx.tx.invocation.method_name = 0;
  stx.tx.invocation.gas_limit = UINT64_MAX;
  stx.tx.invocation.calldata = x23::Vec<uint8_t>(kMaxLogMessageLen + 1, 12);
  stx.nonce = 0;

  SystemCallHandler handler(stx, *snapshot_cache_);

  auto res = handler.root_invoke();

  EXPECT_TRUE(!res.result);

  auto const &logs = handler.get_logs();

  ASSERT_EQ(logs.size(), 0);
}

} // namespace hogd
