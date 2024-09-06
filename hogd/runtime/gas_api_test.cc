#include <gtest/gtest.h>

#include "hogd/base/types.h"
#include "hogd/runtime/host_fn_status.h"
#include "hogd/runtime/test_load_wasm.h"
#include "hogd/runtime/wasmi_runtime.h"

#include <memory>

namespace hogd {

namespace test {

HostFnStatus<uint64_t>
consume_gas_call(HostCallContext *ctxp)
{
  if (!ctxp->runtime->consume_gas(100)) {
    return HostFnError{HostFnErrorCode::kOutOfGas};
  }
  return 0;
}

HostFnStatus<uint64_t>
consume_gas_call2(HostCallContext *ctxp)
{
  if (!ctxp->runtime->consume_gas(5000)) {
    return HostFnError{HostFnErrorCode::kOutOfGas};
  }
  return 0;
}

} // namespace test

class GasApiTest : public ::testing::Test {
protected:
  void SetUp() override
  {
    // this wasm just invokes an external call.  We're just reusing it here.
    auto c = test::load_wasm_from_file("test_error_handling.wasm");

    runtime_ = std::make_unique<WasmiRuntime>(c, nullptr);

    runtime_->template link_fn<&test::consume_gas_call2>(
        "test", "good_call"); // called by "call2"
    runtime_->template link_fn<&test::consume_gas_call>(
        "test", "external_call"); // called by "call1"
  }

  std::unique_ptr<WasmiRuntime> runtime_;
};

TEST_F(GasApiTest, SetGasNoInvoke)
{
  EXPECT_EQ(runtime_->get_available_gas(), 0);

  runtime_->set_available_gas(100);

  EXPECT_TRUE(runtime_->consume_gas(60));

  EXPECT_EQ(runtime_->get_available_gas(), 40);

  EXPECT_FALSE(runtime_->consume_gas(50));

  EXPECT_EQ(runtime_->get_available_gas(), 0);
}

TEST_F(GasApiTest, InvokeResetsGas)
{
  runtime_->set_available_gas(5000);

  auto res = runtime_->invoke("call1", 300);

  EXPECT_TRUE(!!res.result);

  EXPECT_GE(res.gas_consumed, 100);

  EXPECT_EQ(runtime_->get_available_gas(), 5000);

  EXPECT_TRUE(runtime_->consume_gas(res.gas_consumed));

  res = runtime_->invoke("call1", 80);
  EXPECT_FALSE(!!res.result);
  EXPECT_EQ(res.gas_consumed, 80);
}

} // namespace hogd
