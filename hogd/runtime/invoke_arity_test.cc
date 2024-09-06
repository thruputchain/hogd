#include <gtest/gtest.h>

/**
 * Fairly uninteresting as test cases go
 * but this behavior is critical to exactly maintain,
 * and very easy to mess up with a copy-paste bug
 */

#include "hogd/base/types.h"
#include "hogd/runtime/host_fn_status.h"
#include "hogd/runtime/test_load_wasm.h"
#include "hogd/runtime/wasmi_runtime.h"

#include <cstdint>
#include <memory>
#include <stdexcept>

namespace hogd {

namespace test {

HostFnStatus<uint64_t>
arity0(HostCallContext *)
{
  return 100;
}

HostFnStatus<uint64_t>
arity1(HostCallContext *, uint64_t arg0)
{
  if (arg0 != 1) {
    throw std::runtime_error("invalid arity check");
  }
  return 101;
}

HostFnStatus<uint64_t>
arity2(HostCallContext *, uint64_t arg0, uint64_t arg1)
{
  if (arg0 != 1 || arg1 != 2) {
    throw std::runtime_error("invalid arity check");
  }
  return 102;
}

HostFnStatus<uint64_t>
arity3(HostCallContext *, uint64_t arg0, uint64_t arg1, uint64_t arg2)
{
  if (arg0 != 1 || arg1 != 2 || arg2 != 3) {
    throw std::runtime_error("invalid arity check");
  }
  return 103;
}

HostFnStatus<uint64_t>
arity4(HostCallContext *, uint64_t arg0, uint64_t arg1, uint64_t arg2,
       uint64_t arg3)
{
  if (arg0 != 1 || arg1 != 2 || arg2 != 3 || arg3 != 4) {
    throw std::runtime_error("invalid arity check");
  }
  return 104;
}

HostFnStatus<uint64_t>
arity5(HostCallContext *, uint64_t arg0, uint64_t arg1, uint64_t arg2,
       uint64_t arg3, uint64_t arg4)
{
  if (arg0 != 1 || arg1 != 2 || arg2 != 3 || arg3 != 4 || arg4 != 5) {
    throw std::runtime_error("invalid arity check");
  }
  return 105;
}

} // namespace test

class InvokeArityTests : public ::testing::Test {

protected:
  void SetUp() override
  {
    auto c = test::load_wasm_from_file("test_invoke_arity.wasm");

    runtime_ = std::make_unique<WasmiRuntime>(c, nullptr);

    runtime_->template link_fn<&test::arity0>("test", "arg0");
    runtime_->template link_fn<&test::arity1>("test", "arg1");
    runtime_->template link_fn<&test::arity2>("test", "arg2");
    runtime_->template link_fn<&test::arity3>("test", "arg3");
    runtime_->template link_fn<&test::arity4>("test", "arg4");
    runtime_->template link_fn<&test::arity5>("test", "arg5");
  }

  std::unique_ptr<WasmiRuntime> runtime_;
};

TEST_F(InvokeArityTests, arity0)
{
  auto res = runtime_->invoke("calltest0", UINT64_MAX);
  EXPECT_TRUE(!!res.result);
  EXPECT_EQ(*res.result, 100);
}

TEST_F(InvokeArityTests, arity1)
{
  auto res = runtime_->invoke("calltest1", UINT64_MAX);
  EXPECT_TRUE(!!res.result);
  EXPECT_EQ(*res.result, 101);
}

TEST_F(InvokeArityTests, arity2)
{
  auto res = runtime_->invoke("calltest2", UINT64_MAX);
  EXPECT_TRUE(!!res.result);
  EXPECT_EQ(*res.result, 102);
}

TEST_F(InvokeArityTests, arity3)
{
  auto res = runtime_->invoke("calltest3", UINT64_MAX);
  EXPECT_TRUE(!!res.result);
  EXPECT_EQ(*res.result, 103);
}

TEST_F(InvokeArityTests, arity4)
{
  auto res = runtime_->invoke("calltest4", UINT64_MAX);
  EXPECT_TRUE(!!res.result);
  EXPECT_EQ(*res.result, 104);
}

TEST_F(InvokeArityTests, arity5)
{
  auto res = runtime_->invoke("calltest5", UINT64_MAX);
  EXPECT_TRUE(!!res.result);
  EXPECT_EQ(*res.result, 105);
}

} // namespace hogd
