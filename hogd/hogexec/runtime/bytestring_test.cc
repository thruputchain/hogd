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
#include "storage_object.hh"
#include "storage_interface.hh"
#include "transaction.hh"
#include "types.hh"
#include "x23/vec.h"

namespace hogd {

class BytestringTest : public ::testing::Test,
                       public test::RuntimeTestFramework {
protected:
  void SetUp() override
  {
    test::RuntimeTestFramework::initialize();

    auto [h, c] = test::load_built_rs_contract("test_bytestring.wasm");

    contract_map_->add_contract(h, c);
    contract_map_->deploy_contract(addr_, h);
    contract_map_->commit_round();
  }

  ContractAddress addr_ = ContractAddress{};
};

TEST_F(BytestringTest, BasicWrite)
{
  std::vector<uint8_t> bytes = {0, 1, 2, 3};
  StorageKey key = sha256(std::as_bytes(std::span(bytes)));
  std::vector<uint8_t> bstring = {0, 1, 2, 3, 4, 5, 6, 7};

  {
    SignedTransaction stx;
    stx.tx.invocation.invoked_address = addr_;
    stx.tx.invocation.method_name = 0;
    stx.tx.invocation.gas_limit = UINT64_MAX;
    stx.tx.invocation.calldata = test::make_calldata(key, bstring);

    SystemCallHandler handler(stx, *snapshot_cache_);

    auto res = handler.root_invoke();

    EXPECT_TRUE(!!res.result);

    auto deltas = handler.get_deltas();

    ASSERT_EQ(deltas.size(), 1);
    EXPECT_EQ(deltas[0].addr_key, AddressAndKey(addr_, key));
    ASSERT_EQ(deltas[0].deltas.size(), 1);
    EXPECT_EQ(deltas[0].deltas[0].bytestring(),
              Bytestring(test::make_calldata(bstring)));

    auto status =
        get_write_client().try_reserve_transaction_deltas(deltas,
                                                          SHA256Hash{})();
    ASSERT_EQ(status, StorageRPCError::kNone);
    status =
        get_write_client().commit_transaction_deltas(deltas, SHA256Hash{})();
    ASSERT_EQ(status, StorageRPCError::kNone);
  }

  {
    SignedTransaction stx;
    stx.tx.invocation.invoked_address = addr_;
    stx.tx.invocation.method_name = 1;
    stx.tx.invocation.gas_limit = UINT64_MAX;
    stx.tx.invocation.calldata = test::make_calldata(key);

    SystemCallHandler handler(stx, *snapshot_cache_);

    auto res = handler.root_invoke();

    EXPECT_TRUE(!res.result);

    auto deltas = handler.get_deltas();

    ASSERT_EQ(deltas.size(), 0);
  }

  commit_round();

  {
    SignedTransaction stx;
    stx.tx.invocation.invoked_address = addr_;
    stx.tx.invocation.method_name = 1;
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
    check_log_eq(logs[0], test::make_calldata(bstring));
  }
}

} // namespace hogd
