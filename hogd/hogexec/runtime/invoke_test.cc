#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>

#include "hogd/base/types.h"
#include "hogd/crypto/crypto.h"
#include "hogd/hogexec/runtime/storage_snapshot_cache.h"
#include "hogd/hogexec/runtime/syscall_handler.h"
#include "hogd/hogexec/runtime/test_load_wasm.h"
#include "hogd/init.h"
#include "transaction.hh"
#include "types.hh"
#include "x23/vec.h"

namespace hogd {

class InvokeTest : public ::testing::Test {
protected:
  void SetUp() override
  {
    auto c1 = test::load_built_rs_contract("test_invoke_1.wasm");
    auto c2 = test::load_built_rs_contract("test_invoke_2.wasm");

    auto h1 = storage_snapshot_.register_contract(c1);
    auto h2 = storage_snapshot_.register_contract(c2);

    std::vector<uint8_t> bytes1 = {0, 1, 2, 3};
    std::vector<uint8_t> bytes2 = {3, 2, 1, 0};

    addr1_ = sha256(std::as_bytes(std::span(bytes1)));
    addr2_ = sha256(std::as_bytes(std::span(bytes2)));

    storage_snapshot_.deploy_contract(addr1_, h1);
    storage_snapshot_.deploy_contract(addr2_, h2);
  }

  test::LocalMockStorageSnapshot storage_snapshot_;
  ContractAddress addr1_;
  ContractAddress addr2_;

  static void check_log_eq(LogEntry const &entry,
                           x23::Vec<uint8_t> const &calldata)
  {
    ASSERT_EQ(entry.message.size(), calldata.size());
    EXPECT_EQ(0, std::memcmp(entry.message.data(), calldata.data(),
                             calldata.size()));
  }
};

TEST_F(InvokeTest, CalldataPassthroughSuccess)
{
  SignedTransaction stx;
  stx.tx.invocation.invoked_address = addr2_;
  stx.tx.invocation.method_name = 0;
  stx.tx.invocation.gas_limit = UINT64_MAX;
  stx.tx.invocation.calldata =
      test::make_calldata(addr1_, (MethodID)0,
                          std::vector<uint8_t>{0, 1, 2, 3});

  SystemCallHandler handler(stx, storage_snapshot_);

  auto res = handler.root_invoke();

  EXPECT_TRUE(!!res.result);

  auto const &logs = handler.get_logs();

  ASSERT_EQ(logs.size(), 2);

  EXPECT_EQ(logs[0].source_address, addr1_);
  check_log_eq(logs[0], test::make_calldata(addr2_));

  EXPECT_EQ(logs[1].source_address, addr1_);
  check_log_eq(logs[1], test::make_calldata(std::vector<uint8_t>{0, 1, 2, 3}));
}

} // namespace hogd
