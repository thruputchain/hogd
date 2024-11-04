#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <span>
#include <vector>

#include "hogd/base/types.h"
#include "hogd/crypto/crypto.h"
#include "hogd/hogexec/runtime/runtime_test_framework.h"
#include "hogd/hogexec/runtime/syscall_handler.h"
#include "hogd/hogexec/runtime/test_load_wasm.h"
#include "hogd/softstate/local_softstate_map.h"
#include "storage_interface.hh"
#include "storage_object.hh"
#include "transaction.hh"
#include "types.hh"
#include "x23/vec.h"

namespace hogd {

class NNInt64Test : public ::testing::Test, public test::RuntimeTestFramework {
protected:
  void SetUp() override
  {
    test::RuntimeTestFramework::initialize();

    auto [h, c] = test::load_built_rs_contract("test_nnint64.wasm");

    contract_map_->add_contract(h, c);
    contract_map_->deploy_contract(addr_, h);
    contract_map_->commit_round();
  }

  ContractAddress addr_ = ContractAddress{};
};

TEST_F(NNInt64Test, SetAddPosValue)
{
  std::vector<uint8_t> bytes = {0, 1, 2, 3};
  StorageKey key = sha256(std::as_bytes(std::span(bytes)));

  {
    SignedTransaction stx;
    stx.tx.invocation.invoked_address = addr_;
    stx.tx.invocation.method.method_type(MethodSpecifierType::kDefaultUint32);
    stx.tx.invocation.method.method_id_uint32() = 0;
    stx.tx.invocation.gas_limit = UINT64_MAX;
    stx.tx.invocation.calldata = test::make_calldata(key, 100LL, 10LL);

    SystemCallHandler handler(stx, *snapshot_cache_);

    auto res = handler.root_invoke();

    EXPECT_TRUE(!!res.result);

    auto deltas = handler.get_deltas();

    ASSERT_EQ(deltas.size(), 1);
    EXPECT_EQ(deltas[0].addr_key, AddressAndKey(addr_, key));
    ASSERT_EQ(deltas[0].deltas.size(), 1);
    EXPECT_EQ(deltas[0].deltas[0].nnint64(),
              NonnegativeInt64SetAddArgs(100, 10));

    ASSERT_EQ(StorageRPCError::kNone,
              handler.try_reserve_transaction(get_write_client()));

    handler.commit_transaction(get_write_client());
  }

  {
    SignedTransaction stx;
    stx.tx.invocation.invoked_address = addr_;
    stx.tx.invocation.method.method_type(MethodSpecifierType::kDefaultUint32);
    stx.tx.invocation.method.method_id_uint32() = 0;
    stx.tx.invocation.gas_limit = UINT64_MAX;
    stx.tx.invocation.calldata = test::make_calldata(key, 10LL, 10LL);

    SystemCallHandler handler(stx, *snapshot_cache_);

    auto res = handler.root_invoke();

    EXPECT_TRUE(!!res.result);

    auto deltas = handler.get_deltas();

    ASSERT_EQ(deltas.size(), 1);
    EXPECT_EQ(deltas[0].addr_key, AddressAndKey(addr_, key));
    ASSERT_EQ(deltas[0].deltas.size(), 1);
    EXPECT_EQ(deltas[0].deltas[0].nnint64(),
              NonnegativeInt64SetAddArgs(10, 10));

    ASSERT_NE(StorageRPCError::kNone,
              handler.try_reserve_transaction(get_write_client()));
  }

  commit_round();

  {
    SignedTransaction stx;
    stx.tx.invocation.invoked_address = addr_;
    stx.tx.invocation.method.method_type(MethodSpecifierType::kDefaultUint32);
    stx.tx.invocation.method.method_id_uint32() = 1;
    stx.tx.invocation.gas_limit = UINT64_MAX;
    stx.tx.invocation.calldata = test::make_calldata(key);

    SystemCallHandler handler(stx, *snapshot_cache_);

    auto res = handler.root_invoke();

    EXPECT_TRUE(!!res.result);

    auto deltas = handler.get_deltas();

    ASSERT_EQ(deltas.size(), 0);

    auto logs = handler.get_logs();

    ASSERT_EQ(logs.size(), 1);

    EXPECT_EQ(logs[0].source_address, addr_);
    check_log_eq(logs[0], test::make_calldata(110LL));
  }
}

TEST_F(NNInt64Test, NegativeSet)
{
  std::vector<uint8_t> bytes = {0, 1, 2, 3};
  StorageKey key = sha256(std::as_bytes(std::span(bytes)));

  auto make_set_add_tx = [&](int64_t set, int64_t add) {
    SignedTransaction stx;
    stx.tx.invocation.invoked_address = addr_;
    stx.tx.invocation.method.method_type(MethodSpecifierType::kDefaultUint32);
    stx.tx.invocation.method.method_id_uint32() = 0;
    stx.tx.invocation.gas_limit = UINT64_MAX;
    stx.tx.invocation.calldata = test::make_calldata(key, set, add);

    return SystemCallHandler(stx, *snapshot_cache_);
  };

  auto expect_success = [&](SystemCallHandler handler) {
    ASSERT_TRUE(!!handler.root_invoke().result);

    ASSERT_EQ(StorageRPCError::kNone,
              handler.try_reserve_transaction(get_write_client()));

    handler.commit_transaction(get_write_client());
  };

  auto expect_fail = [&](SystemCallHandler handler) {
    EXPECT_FALSE(!!handler.root_invoke().result);

    auto const &deltas = handler.get_deltas();

    EXPECT_EQ(deltas.size(), 0);
  };

  auto expect_apply_fail = [&](SystemCallHandler handler) {
    EXPECT_TRUE(!!handler.root_invoke().result);

    auto const &deltas = handler.get_deltas();
    ASSERT_EQ(deltas.size(), 1);

    EXPECT_NE(StorageRPCError::kNone,
              handler.try_reserve_transaction(get_write_client()));
  };

  expect_success(make_set_add_tx(-100, 30));
  expect_success(make_set_add_tx(-100, 0));
  expect_fail(make_set_add_tx(-100, -10));
  expect_apply_fail(make_set_add_tx(100, 10));

  commit_round();

  SignedTransaction stx;
  stx.tx.invocation.invoked_address = addr_;
  stx.tx.invocation.method.method_type(MethodSpecifierType::kDefaultUint32);
  stx.tx.invocation.method.method_id_uint32() = 1;
  stx.tx.invocation.gas_limit = UINT64_MAX;
  stx.tx.invocation.calldata = test::make_calldata(key);

  SystemCallHandler handler(stx, *snapshot_cache_);

  auto res = handler.root_invoke();

  EXPECT_TRUE(!!res.result);

  auto logs = handler.get_logs();

  ASSERT_EQ(logs.size(), 1);

  EXPECT_EQ(logs[0].source_address, addr_);
  check_log_eq(logs[0], test::make_calldata(-70LL));
}

} // namespace hogd
