#include <cstdint>
#include <memory>
#include <stdexcept>

#include <gtest/gtest.h>

#include "contract.hh"
#include "hogd/hogexec/runtime/runtime.h"
#include "hogd/hogexec/runtime/status.h"
#include "hogd/hogexec/runtime/test_load_wasm.h"
#include "hogd/hogexec/runtime/wasmi_runtime.h"

namespace hogd {

namespace {

HostFnResult
throw_runtime_error(HostCallContext * /*unused*/)
{
  throw std::runtime_error("bad fn\n");
}

HostFnResult
throw_unrecoverable_host_error(HostCallContext * /*unused*/)
{
  return {HostFnErrorCode::kUnrecoverable, 0};
}

HostFnResult
throw_bad_alloc(HostCallContext * /*unused*/)
{
  throw std::bad_alloc();
}

HostFnResult
good_call(HostCallContext * /*unused*/)
{
  return {HostFnErrorCode::kNoneOrRecoverable, 10};
}

HostFnResult
exit_call(HostCallContext * /*unused*/)
{
  return {HostFnErrorCode::kExit, 10};
}

HostFnResult
return_syscall(HostCallContext * /*unused*/)
{
  return {HostFnErrorCode::kReturn, 0};
}

// Wasmi doesn't necessarily insert a "consume gas" instruction into its
// internal bytecode at the end of a call instruction. So if a host function
// uses all gas, it has to return kOutOfGas.
HostFnResult
consume_too_much_gas(HostCallContext *context)
{
  bool _ = context->runtime->consume_gas(context->runtime->get_available_gas());
  return {HostFnErrorCode::kOutOfGas, 0};
}

namespace {
Runtime *s_runtime = nullptr;
bool reentrance_hit = false;
} // namespace

HostFnResult
reentrance(HostCallContext * /*unused*/)
{
  reentrance_hit = true;

  auto res = s_runtime->invoke("unreachable", UINT64_MAX);
  if (!res.result.has_value()) {
    return {HostFnErrorCode::kUnrecoverable, 0};
  }
  return {HostFnErrorCode::kNoneOrRecoverable, 0};
}

} // namespace

class WasmiErrorHandlingTest : public ::testing::Test {
protected:
  void SetUp() override
  {
    contract_ = test::load_wasm_from_file("test_error_handling.wasm");
    context_.link_fn("test", "good_call", &good_call);
  }
  ContractBlob contract_;
  WasmiContext context_;
  std::unique_ptr<Runtime> runtime_;
};

TEST_F(WasmiErrorHandlingTest, UnlinkedFn)
{
  runtime_ = context_.make_new_runtime(contract_, nullptr, true);
  EXPECT_FALSE(!!runtime_);
}

// make sure we don't break if errors do get thrown
TEST_F(WasmiErrorHandlingTest, RuntimeError)
{
  context_.link_fn("test", "external_call", &throw_runtime_error);
  runtime_ = context_.make_new_runtime(contract_, nullptr, true);

  ASSERT_TRUE(!!runtime_);

  auto res = runtime_->invoke("call1", UINT64_MAX);

  EXPECT_FALSE(!!res.result);
  EXPECT_EQ(res.result.error(), InvokeError{InvokeErrorCode::kUnrecoverable});
}

TEST_F(WasmiErrorHandlingTest, ExitHandling)
{
  context_.link_fn("test", "external_call", &exit_call);
  runtime_ = context_.make_new_runtime(contract_, nullptr, true);

  ASSERT_TRUE(!!runtime_);

  auto res = runtime_->invoke("call1", UINT64_MAX);

  EXPECT_TRUE(!!res.result);
  EXPECT_EQ(*res.result, 10);
}

TEST_F(WasmiErrorHandlingTest, HostError)
{
  context_.link_fn("test", "external_call", &throw_unrecoverable_host_error);
  runtime_ = context_.make_new_runtime(contract_, nullptr, true);

  ASSERT_TRUE(!!runtime_);

  auto res = runtime_->invoke("call1", UINT64_MAX);

  EXPECT_EQ(res.result.error(), InvokeError{InvokeErrorCode::kUnrecoverable});
}

TEST_F(WasmiErrorHandlingTest, OtherWeirdError)
{
  context_.link_fn("test", "external_call", &throw_bad_alloc);
  runtime_ = context_.make_new_runtime(contract_, nullptr, true);

  ASSERT_TRUE(!!runtime_);

  auto res = runtime_->invoke("call1", UINT64_MAX);

  EXPECT_FALSE(!!res.result);
  EXPECT_EQ(res.result.error(), InvokeError{InvokeErrorCode::kUnrecoverable});
}

TEST_F(WasmiErrorHandlingTest, WasmError)
{
  // must link all the fns, required for linking
  context_.link_fn("test", "external_call", &good_call);
  runtime_ = context_.make_new_runtime(contract_, nullptr, true);

  ASSERT_TRUE(!!runtime_);

  auto res = runtime_->invoke("call1", UINT64_MAX);

  EXPECT_TRUE(!!res.result);
  EXPECT_EQ(*res.result, 0);

  res = runtime_->invoke("unreachable", UINT64_MAX);

  EXPECT_FALSE(!!res.result);
  EXPECT_EQ(res.result.error(), InvokeError{InvokeErrorCode::kDeterministic});
}

TEST_F(WasmiErrorHandlingTest, NonexistFn)
{
  // must link all the fns, required for linking
  context_.link_fn("test", "external_call", &good_call);
  runtime_ = context_.make_new_runtime(contract_, nullptr, true);

  ASSERT_TRUE(!!runtime_);

  auto res = runtime_->invoke("call1", UINT64_MAX);

  EXPECT_TRUE(!!res.result);

  res = runtime_->invoke("blah", UINT64_MAX);

  EXPECT_FALSE(!!res.result);
  EXPECT_EQ(res.result.error(), InvokeError{InvokeErrorCode::kDeterministic});
}

TEST_F(WasmiErrorHandlingTest, FollowBadWithGood)
{
  context_.link_fn("test", "external_call", &throw_unrecoverable_host_error);
  runtime_ = context_.make_new_runtime(contract_, nullptr, true);

  ASSERT_TRUE(!!runtime_);

  auto res = runtime_->invoke("call1", UINT64_MAX);

  EXPECT_FALSE(!!res.result);
  EXPECT_EQ(res.result.error(), InvokeError{InvokeErrorCode::kUnrecoverable});

  res = runtime_->invoke("call2", UINT64_MAX);

  EXPECT_TRUE(!!res.result);
}

TEST_F(WasmiErrorHandlingTest, CheckReturn)
{
  context_.link_fn("test", "external_call", &return_syscall);
  runtime_ = context_.make_new_runtime(contract_, nullptr, true);

  ASSERT_TRUE(!!runtime_);

  auto res = runtime_->invoke("call_then_unreachable", UINT64_MAX);

  EXPECT_TRUE(!!res.result);
}

TEST_F(WasmiErrorHandlingTest, FollowReallyBadWithGood)
{
  context_.link_fn("test", "external_call", &throw_bad_alloc);
  runtime_ = context_.make_new_runtime(contract_, nullptr, true);

  ASSERT_TRUE(!!runtime_);

  auto res = runtime_->invoke("call1", UINT64_MAX);

  EXPECT_FALSE(!!res.result);
  EXPECT_EQ(res.result.error(), InvokeError{InvokeErrorCode::kUnrecoverable});

  res = runtime_->invoke("call2", UINT64_MAX);

  EXPECT_TRUE(!!res.result);
}

TEST_F(WasmiErrorHandlingTest, Reentrance)
{
  context_.link_fn("test", "external_call", &reentrance);
  runtime_ = context_.make_new_runtime(contract_, nullptr, true);

  ASSERT_TRUE(!!runtime_);

  s_runtime = runtime_.get();
  reentrance_hit = false;

  auto res = runtime_->invoke("call1", UINT64_MAX);

  EXPECT_FALSE(!!res.result);

  EXPECT_TRUE(reentrance_hit);
}

TEST_F(WasmiErrorHandlingTest, NullHandling)
{
  ContractBlob null_contract;
  runtime_ = context_.make_new_runtime(null_contract, nullptr, true);

  EXPECT_FALSE(!!runtime_);
}

TEST_F(WasmiErrorHandlingTest, NullLinkHandling)
{
  // The WASM standard accepts empty strings, but we ban that out to keep the
  // ffi handling simpler (meaning we never have a reason to pass a null string
  // across FFI).
  EXPECT_FALSE(context_.link_fn("", "good_call", &good_call));
  EXPECT_FALSE(context_.link_fn("test", "", &good_call));
  EXPECT_TRUE(context_.link_fn("foo", "good_call", &good_call));

  runtime_ = context_.make_new_runtime(contract_, nullptr, true);

  EXPECT_FALSE(!!runtime_);
}

TEST_F(WasmiErrorHandlingTest, OutOfGas)
{
  context_.link_fn("test", "external_call", &consume_too_much_gas);
  runtime_ = context_.make_new_runtime(contract_, nullptr, true);

  ASSERT_TRUE(!!runtime_);

  auto res = runtime_->invoke("call1", UINT64_MAX);

  EXPECT_FALSE(!!res.result);
  EXPECT_EQ(res.result.error(), InvokeError{InvokeErrorCode::kOutOfGas});

  res = runtime_->invoke("call1", 1);

  EXPECT_FALSE(!!res.result);
  EXPECT_EQ(res.result.error(), InvokeError{InvokeErrorCode::kOutOfGas});
}

TEST_F(WasmiErrorHandlingTest, NoGas)
{
  context_.link_fn("test", "external_call", &good_call);
  runtime_ = context_.make_new_runtime(contract_, nullptr, true);

  ASSERT_TRUE(!!runtime_);

  auto res = runtime_->invoke("call1", 1);

  EXPECT_FALSE(!!res.result);
  EXPECT_EQ(res.result.error(), InvokeError{InvokeErrorCode::kOutOfGas});
}

TEST_F(WasmiErrorHandlingTest, Spin)
{
  context_.link_fn("test", "external_call", &good_call);
  runtime_ = context_.make_new_runtime(contract_, nullptr, true);

  ASSERT_TRUE(!!runtime_);

  auto res = runtime_->invoke("spin", 10000);

  EXPECT_FALSE(!!res.result);
  EXPECT_EQ(res.result.error(), InvokeError{InvokeErrorCode::kOutOfGas});
}

} // namespace hogd
