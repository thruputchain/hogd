// Fairly uninteresting as test cases go but this behavior is critical to
// exactly maintain, and very easy to mess up with a copy-paste bug.

#include <cstdint>
#include <optional>
#include <stdexcept>

#include <gtest/gtest.h>

#include "ffi_enums.hh"
#include "hogd/base/types.h"
#include "hogd/hogexec/runtime/status.h"
#include "hogd/hogexec/runtime/test_load_wasm.h"
#include "hogd/hogexec/runtime/wasmi_runtime.h"

namespace hogd {

namespace {

HostFnResult
arity0(HostCallContext * /*unused*/)
{
  return {HostFnErrorCode::kNoneOrRecoverable, 100};
}

HostFnResult
arity1(HostCallContext * /*unused*/, uint64_t arg0)
{
  if (arg0 != 1) {
    throw std::runtime_error("invalid arity check");
  }
  return {HostFnErrorCode::kNoneOrRecoverable, 101};
}

HostFnResult
arity2(HostCallContext * /*unused*/, uint64_t arg0, uint64_t arg1)
{
  if (arg0 != 1 || arg1 != 2) {
    throw std::runtime_error("invalid arity check");
  }
  return {HostFnErrorCode::kNoneOrRecoverable, 102};
}

HostFnResult
arity3(HostCallContext * /*unused*/, uint64_t arg0, uint64_t arg1,
       uint64_t arg2)
{
  if (arg0 != 1 || arg1 != 2 || arg2 != 3) {
    throw std::runtime_error("invalid arity check");
  }
  return {HostFnErrorCode::kNoneOrRecoverable, 103};
}

HostFnResult
arity4(HostCallContext * /*unused*/, uint64_t arg0, uint64_t arg1,
       uint64_t arg2, uint64_t arg3)
{
  if (arg0 != 1 || arg1 != 2 || arg2 != 3 || arg3 != 4) {
    throw std::runtime_error("invalid arity check");
  }
  return {HostFnErrorCode::kNoneOrRecoverable, 104};
}

HostFnResult
arity5(HostCallContext * /*unused*/, uint64_t arg0, uint64_t arg1,
       uint64_t arg2, uint64_t arg3, uint64_t arg4)
{
  if (arg0 != 1 || arg1 != 2 || arg2 != 3 || arg3 != 4 || arg4 != 5) {
    throw std::runtime_error("invalid arity check");
  }
  return {HostFnErrorCode::kNoneOrRecoverable, 105};
}

} // namespace

class InvokeArityTests : public ::testing::Test {

protected:
  void SetUp() override
  {
    auto c = test::load_wasm_from_file("test_invoke_arity.wasm");

    WasmiContext context;

    context.link_fn("test", "arg0", &arity0);
    context.link_fn("test", "arg1", &arity1);
    context.link_fn("test", "arg2", &arity2);
    context.link_fn("test", "arg3", &arity3);
    context.link_fn("test", "arg4", &arity4);
    context.link_fn("test", "arg5", &arity5);
    runtime_ = context.make_new_runtime(c, nullptr);
    ASSERT_TRUE(runtime_);
  }

  std::optional<WasmiRuntime> runtime_;
};

// NOLINTBEGIN(bugprone-unchecked-optional-access)
TEST_F(InvokeArityTests, arity0)
{
  auto res = runtime_->invoke("calltest0", UINT64_MAX);
  EXPECT_TRUE(res.result_);
  EXPECT_EQ(*res.result_, 100);
}

TEST_F(InvokeArityTests, arity1)
{
  auto res = runtime_->invoke("calltest1", UINT64_MAX);
  EXPECT_TRUE(res.result_);
  EXPECT_EQ(*res.result_, 101);
}

TEST_F(InvokeArityTests, arity2)
{
  auto res = runtime_->invoke("calltest2", UINT64_MAX);
  EXPECT_TRUE(res.result_);
  EXPECT_EQ(*res.result_, 102);
}

TEST_F(InvokeArityTests, arity3)
{
  auto res = runtime_->invoke("calltest3", UINT64_MAX);
  EXPECT_TRUE(res.result_);
  EXPECT_EQ(*res.result_, 103);
}

TEST_F(InvokeArityTests, arity4)
{
  auto res = runtime_->invoke("calltest4", UINT64_MAX);
  EXPECT_TRUE(res.result_);
  EXPECT_EQ(*res.result_, 104);
}

TEST_F(InvokeArityTests, arity5)
{
  auto res = runtime_->invoke("calltest5", UINT64_MAX);
  EXPECT_TRUE(res.result_);
  EXPECT_EQ(*res.result_, 105);
}
// NOLINTEND(bugprone-unchecked-optional-access)

} // namespace hogd
