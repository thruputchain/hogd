#include <gtest/gtest.h>

#include <memory>
#include <utility>

#include "contract.hh"
#include "ffi_enums.hh"
#include "hogd/base/types.h"
#include "hogd/crypto/crypto.h"
#include "hogd/hogexec/common/status.h"
#include "hogd/hogexec/runtime/runtime.h"
#include "hogd/hogexec/runtime/syscall_entry.h"
#include "hogd/hogexec/runtime/trampoline_result.h"
#include "hogd/hogexec/runtime/wasm/test_load_wasm.h"
#include "hogd/hogexec/runtime/wasm/wasm_test_framework.h"
#include "syscall_error.hh"

namespace hogd {

namespace {

TrampolineResult
error(HostFnErrorCode const &code)
{
  return to_trampoline_result(HostFnError{code});
}

TrampolineResult
success()
{
  return to_trampoline_result({SyscallErrorCode::kSyscallErrorCodeNone});
}

extern "C" TrampolineResult
consume_gas_call(HostCallContext *ctxp) noexcept
{
  if (!ctxp->runtime->consume_gas(100)) {
    return error(HostFnErrorCode::OutOfGas);
  }
  return success();
}

extern "C" TrampolineResult
consume_gas_call2(HostCallContext *ctxp) noexcept
{
  if (!ctxp->runtime->consume_gas(5000)) {
    return error(HostFnErrorCode::OutOfGas);
  }
  return success();
}

} // namespace

class GasApiTest : public ::testing::TestWithParam<WasmEngine>,
                   public WasmTestFramework {
protected:
  void SetUp() override
  {
    setup_wasm_context(GetParam());
    Contract contract(ContractType::kWasmi);
    // this wasm just invokes an external call.  We're just reusing it here.
    contract.wasmi_contract() =
        test::load_wasm_from_file("test_error_handling.wasm");

    auto deployed_contract_hash = DeployedContractHash{
        .hash = sha256(contract),
        .contract_size =
            static_cast<uint32_t>(contract.wasmi_contract().wasm.size()),
        .contract_type = ContractType::kWasmi,
    };

    link_fn("test", "good_call",
            &consume_gas_call2); // called by "call2"
    link_fn("test", "external_call",
            &consume_gas_call); // called by "call1"

    make_syscall_handler(contract);

    auto runtime_res = get_context()->make_new_runtime(deployed_contract_hash,
                                                       get_syscall_handler());
    ASSERT_TRUE(runtime_res.has_value());
    runtime_ = std::move(runtime_res.value());

    ASSERT_TRUE(!!runtime_);
  }

  std::unique_ptr<Runtime> runtime_;
};

TEST_P(GasApiTest, SetGasNoInvoke)
{
  EXPECT_EQ(runtime_->get_available_gas(), 0);

  runtime_->set_available_gas(100);

  EXPECT_TRUE(runtime_->consume_gas(60));

  EXPECT_EQ(runtime_->get_available_gas(), 40);

  EXPECT_FALSE(runtime_->consume_gas(50));

  EXPECT_EQ(runtime_->get_available_gas(), 0);
}

TEST_P(GasApiTest, InvokeResetsGas)
{
  runtime_->set_available_gas(5000);

  auto res = runtime_->invoke("call1", 300);

  EXPECT_TRUE(res.result_);

  EXPECT_GE(res.gas_consumed_, 100);

  EXPECT_EQ(runtime_->get_available_gas(), 5000);

  EXPECT_TRUE(runtime_->consume_gas(res.gas_consumed_));

  res = runtime_->invoke("call1", 80);
  EXPECT_FALSE(res.result_);
  EXPECT_EQ(res.gas_consumed_, 80);
}

INSTANTIATE_TEST_SUITE_P(WasmEngines, GasApiTest,
                         ::testing::Values(WasmEngine::kWasmi,
                                           WasmEngine::kWasmtime),
                         [](const ::testing::TestParamInfo<WasmEngine> &info) {
                           return WasmTestFramework::engine_to_string(
                               info.param);
                         });

} // namespace hogd
