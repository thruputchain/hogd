// Fairly uninteresting as test cases go but this behavior is critical to
// exactly maintain, and very easy to mess up with a copy-paste bug.

#include <cstdint>
#include <memory>
#include <stdexcept>

#include <gtest/gtest.h>

#include "contract.hh"
#include "ffi_enums.hh"
#include "hogd/base/types.h"
#include "hogd/hogexec/runtime/runtime.h"
#include "hogd/hogexec/runtime/status.h"
#include "hogd/hogexec/runtime/test_load_wasm.h"
#include "hogd/hogexec/runtime/wasmi_runtime.h"

namespace hogd {

namespace {

HostFnResult
arity0(HostCallContext * /*unused*/)
{
  return {HostFnErrorCode::kHostNoneOrRecoverable, 100};
}

HostFnResult
arity1(HostCallContext * /*unused*/, uint64_t arg0)
{
  if (arg0 != 1) {
    throw std::runtime_error("invalid arity check");
  }
  return {HostFnErrorCode::kHostNoneOrRecoverable, 101};
}

HostFnResult
arity2(HostCallContext * /*unused*/, uint64_t arg0, uint64_t arg1)
{
  if (arg0 != 1 || arg1 != 2) {
    throw std::runtime_error("invalid arity check");
  }
  return {HostFnErrorCode::kHostNoneOrRecoverable, 102};
}

HostFnResult
arity3(HostCallContext * /*unused*/, uint64_t arg0, uint64_t arg1,
       uint64_t arg2)
{
  if (arg0 != 1 || arg1 != 2 || arg2 != 3) {
    throw std::runtime_error("invalid arity check");
  }
  return {HostFnErrorCode::kHostNoneOrRecoverable, 103};
}

HostFnResult
arity4(HostCallContext * /*unused*/, uint64_t arg0, uint64_t arg1,
       uint64_t arg2, uint64_t arg3)
{
  if (arg0 != 1 || arg1 != 2 || arg2 != 3 || arg3 != 4) {
    throw std::runtime_error("invalid arity check");
  }
  return {HostFnErrorCode::kHostNoneOrRecoverable, 104};
}

HostFnResult
arity5(HostCallContext * /*unused*/, uint64_t arg0, uint64_t arg1,
       uint64_t arg2, uint64_t arg3, uint64_t arg4)
{
  if (arg0 != 1 || arg1 != 2 || arg2 != 3 || arg3 != 4 || arg4 != 5) {
    throw std::runtime_error("invalid arity check");
  }
  return {HostFnErrorCode::kHostNoneOrRecoverable, 105};
}

HostFnResult
arity6(HostCallContext * /*unused*/, uint64_t arg0, uint64_t arg1,
       uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
  if (arg0 != 1 || arg1 != 2 || arg2 != 3 || arg3 != 4 || arg4 != 5 ||
      arg5 != 6) {
    throw std::runtime_error("invalid arity check");
  }
  return {HostFnErrorCode::kHostNoneOrRecoverable, 106};
}

HostFnResult
arity7(HostCallContext * /*unused*/, uint64_t arg0, uint64_t arg1,
       uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5,
       uint64_t arg6)
{
  if (arg0 != 1 || arg1 != 2 || arg2 != 3 || arg3 != 4 || arg4 != 5 ||
      arg5 != 6 || arg6 != 7) {
    throw std::runtime_error("invalid arity check");
  }
  return {HostFnErrorCode::kHostNoneOrRecoverable, 107};
}

HostFnResult
arity8(HostCallContext * /*unused*/, uint64_t arg0, uint64_t arg1,
       uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5,
       uint64_t arg6, uint64_t arg7)
{
  if (arg0 != 1 || arg1 != 2 || arg2 != 3 || arg3 != 4 || arg4 != 5 ||
      arg5 != 6 || arg6 != 7 || arg7 != 8) {
    throw std::runtime_error("invalid arity check");
  }
  return {HostFnErrorCode::kHostNoneOrRecoverable, 108};
}

} // namespace

class InvokeArityTests : public ::testing::Test {

protected:
  void SetUp() override
  {
    Contract contract(ContractType::kWasmi);
    contract.wasmi_contract() =
        test::load_wasm_from_file("test_invoke_arity.wasm");

    WasmiContext context;

    context.link_fn("test", "arg0", &arity0);
    context.link_fn("test", "arg1", &arity1);
    context.link_fn("test", "arg2", &arity2);
    context.link_fn("test", "arg3", &arity3);
    context.link_fn("test", "arg4", &arity4);
    context.link_fn("test", "arg5", &arity5);
    context.link_fn("test", "arg6", &arity6);
    context.link_fn("test", "arg7", &arity7);
    context.link_fn("test", "arg8", &arity8);
    runtime_ = context.make_new_runtime(contract, nullptr, true);
    ASSERT_TRUE(runtime_);
  }

  std::unique_ptr<Runtime> runtime_;
};

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

TEST_F(InvokeArityTests, arity6)
{
  auto res = runtime_->invoke("calltest6", UINT64_MAX);
  EXPECT_TRUE(res.result_);
  EXPECT_EQ(*res.result_, 106);
}

TEST_F(InvokeArityTests, arity7)
{
  auto res = runtime_->invoke("calltest7", UINT64_MAX);
  EXPECT_TRUE(res.result_);
  EXPECT_EQ(*res.result_, 107);
}

TEST_F(InvokeArityTests, arity8)
{
  auto res = runtime_->invoke("calltest8", UINT64_MAX);
  EXPECT_TRUE(res.result_);
  EXPECT_EQ(*res.result_, 108);
}

} // namespace hogd
