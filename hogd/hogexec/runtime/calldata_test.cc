#include <gtest/gtest.h>

#include <cstdint>
#include <memory>

#include "hogd/base/types.h"
#include "hogd/hogexec/runtime/storage_snapshot_cache.h"
#include "hogd/hogexec/runtime/syscall_handler.h"
#include "hogd/hogexec/runtime/test_load_wasm.h"

#include "contract.hh"
#include "transaction.hh"
#include "types.hh"

namespace hogd {

class CalldataTest : public ::testing::Test {
protected:
  void SetUp() override
  {
    // this wasm just invokes an external call.  We're just reusing it here.
    auto c = test::load_built_rs_contract("test_calldata.wasm");

    auto contract_hash = storage_snapshot_.register_contract(c);
    storage_snapshot_.deploy_contract(addr_, contract_hash);
  }

  test::LocalMockStorageSnapshot storage_snapshot_;
  ContractAddress addr_ = ContractAddress{};

  void check_log_eq(LogEntry const &entry, x23::Vec<uint8_t> const &calldata)
  {
    ASSERT_EQ(entry.message.size(), calldata.size());
    EXPECT_EQ(0, std::memcmp(entry.message.data(), calldata.data(),
                             calldata.size()));
  }
};

TEST_F(CalldataTest, NoCalldata)
{

  SignedTransaction stx;
  stx.tx.invocation.invoked_address = addr_;
  stx.tx.invocation.method_name = 0;
  stx.tx.invocation.gas_limit = UINT64_MAX;

  SystemCallHandler handler(stx, storage_snapshot_);

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

  SystemCallHandler handler(stx, storage_snapshot_);

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

  SystemCallHandler handler(stx, storage_snapshot_);

  auto res = handler.root_invoke();

  EXPECT_TRUE(!res.result);

  auto const &logs = handler.get_logs();

  ASSERT_EQ(logs.size(), 0);
}

} // namespace hogd
