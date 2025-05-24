#include <cstdint>
#include <gtest/gtest.h>
#include <memory>

#include "hogd/crypto/crypto.h"
#include "hogd/hogexec/preset_contracts/shared.h"
#include "hogd/hogexec/runtime/runtime.h"
#include "hogd/hogexec/runtime/native_contract_test_framework.h"
#include "hogd/hogexec/runtime/test_load_wasm.h"
#include "transaction.hh"
#include "test_contracts/integration_test_contracts/fibonacci/fibonacci.h"

namespace hogd {

class FibonacciTest : public ::testing::Test, public NativeContractTestFramework {
protected:
  void SetUp() override {
    wasm_runtime_context_ = make_hogexec_wasm_context(RuntimeEngine::kWasmi);
  }
  std::shared_ptr<RuntimeContext> wasm_runtime_context_;
};

TEST_F(FibonacciTest, Fibonacci) {
    auto contract = test::load_test_contract("fibonacci.wasm");
    auto contract_addr = make_contract_address(0);
    in_memory_db_.write_contract(sha256(*contract), contract);
    in_memory_db_.write_deployment(contract_addr, sha256(*contract));
    setup_grant_storage_token(contract_addr, 1000'000'000);

    static_assert(ContractTraitMeta<Fibonacci>::export_index<0>() == 1);
    static_assert(ContractTraitMeta<Fibonacci>::export_index<1>() == 0);
    static_assert(ContractTraitMeta<Fibonacci>::export_index<2>() == 2);

    auto method = ContractTraitMeta<Fibonacci>::method_id("fibonacci");

    auto handler = make_new_system_call_handler(
        SignedTransaction{
            .tx = Transaction{.invocation =
                                    Invocation{.invoked_address = contract_addr,
                                            .method = 3,
                                            .gas_limit = UINT64_MAX,
                                            .calldata = {}}}},
        wasm_runtime_context_);
    auto res = handler.root_invoke();
    ASSERT_TRUE(res.result_);
    ASSERT_EQ(*res.result_, 0);
    auto commit_res = handler.try_commit_transaction()();
    ASSERT_TRUE(commit_res.has_value());
    ASSERT_TRUE(*commit_res);


}

} // namespace hogd