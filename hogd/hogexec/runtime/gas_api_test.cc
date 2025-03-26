#include <gtest/gtest.h>

#include <memory>

#include "ffi_enums.hh"
#include "hogd/base/types.h"
#include "hogd/hogexec/runtime/runtime.h"
#include "hogd/hogexec/runtime/status.h"
#include "hogd/hogexec/runtime/test_load_wasm.h"

namespace hogd {

namespace {

HostFnResult
error(HostFnErrorCode const &code)
{
  return {code, 0};
}

HostFnResult
success(int32_t value)
{
  return {HostFnErrorCode::kNoneOrRecoverable, value};
}

HostFnResult
consume_gas_call(HostCallContext *ctxp)
{
  if (!ctxp->runtime->consume_gas(100)) {
    return error(HostFnErrorCode::kOutOfGas);
  }
  return success(0);
}

HostFnResult
consume_gas_call2(HostCallContext *ctxp)
{
  if (!ctxp->runtime->consume_gas(5000)) {
    return error(HostFnErrorCode::kOutOfGas);
  }
  return success(0);
}

} // namespace

class GasApiTest : public ::testing::TestWithParam<RuntimeEngine> {
protected:
  void SetUp() override
  {
    // this wasm just invokes an external call.  We're just reusing it here.
    auto c = test::load_wasm_from_file("test_error_handling.wasm");

    auto context = make_runtime_context(GetParam());

    context->link_fn("test", "good_call",
                     &consume_gas_call2); // called by "call2"
    context->link_fn("test", "external_call",
                     &consume_gas_call); // called by "call1"

    runtime_ = context->make_new_runtime(c, nullptr, true);

    ASSERT_TRUE(!!runtime_);
  }

  std::unique_ptr<Runtime> runtime_;
};

// NOLINTBEGIN(bugprone-unchecked-optional-access)
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

INSTANTIATE_TEST_SUITE_P(AllEngines, GasApiTest,
                         ::testing::Values(RuntimeEngine::kWasmi,
                                           RuntimeEngine::kWasm3),
                         [](const testing::TestParamInfo<RuntimeEngine> &info) {
                           return engine_to_string(info.param);
                         });

} // namespace hogd
