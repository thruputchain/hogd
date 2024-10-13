#include <gtest/gtest.h>

#include <sys/socket.h>

#include <cstdint>
#include <cstring>

#include "hogd/base/types.h"
#include "hogd/crypto/crypto.h"
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

class NNInt64Test : public ::testing::Test,
                       public test::RuntimeTestFramework {
protected:
  void SetUp() override
  {
    test::RuntimeTestFramework::initialize();

    auto [h, c] = test::load_built_rs_contract("test_nnint64.wasm");

    contract_map_->add_contract(h, c);
    contract_map_->deploy_contract(addr_, h);
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
    stx.tx.invocation.method_name = 0;
    stx.tx.invocation.gas_limit = UINT64_MAX;
    stx.tx.invocation.calldata = test::make_calldata(key, 100LL, 10LL);

    SystemCallHandler handler(stx, *snapshot_cache_);

    auto res = handler.root_invoke();

    EXPECT_TRUE(!!res.result);

    std::printf("wat\n");

    auto deltas = handler.get_deltas();

    std::printf("made deltas\n");

    ASSERT_EQ(deltas.size(), 1);
    EXPECT_EQ(deltas[0].addr_key, AddressAndKey(addr_, key));
    ASSERT_EQ(deltas[0].deltas.size(), 1);
    EXPECT_EQ(deltas[0].deltas[0].nnint64(),
              NonnegativeInt64SetAddArgs(100, 10));

    auto status =
        get_write_client().try_reserve_transaction_deltas(deltas,
                                                          SHA256Hash{})();
    ASSERT_EQ(status, StorageRPCError::kNone);
    status =
        get_write_client().commit_transaction_deltas(deltas, SHA256Hash{})();
    ASSERT_EQ(status, StorageRPCError::kNone);
  }
  std::printf("got here\n");

  {
    SignedTransaction stx;
    stx.tx.invocation.invoked_address = addr_;
    stx.tx.invocation.method_name = 0;
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
      get_write_client().try_reserve_transaction_deltas(deltas, SHA256Hash{})());
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
    check_log_eq(logs[0], test::make_calldata(110LL));
  }
}

} // namespace hogd
