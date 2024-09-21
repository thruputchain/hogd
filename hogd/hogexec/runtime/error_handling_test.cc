#include <cstdint>
#include <memory>
#include <stdexcept>
#include <vector>

#include <gtest/gtest.h>

#include "hogd/base/types.h"
#include "hogd/hogexec/runtime/status.h"
#include "hogd/hogexec/runtime/test_load_wasm.h"
#include "hogd/hogexec/runtime/wasmi_runtime.h"

namespace hogd {

namespace test {

HostFnStatus<uint64_t>
throw_runtime_error(HostCallContext * /*unused*/)
{
  throw std::runtime_error("bad fn\n");
}

HostFnStatus<uint64_t>
throw_unrecoverable_host_error(HostCallContext * /*unused*/)
{
  return HostFnError{HostFnErrorCode::kUnrecoverableUnknown};
}

HostFnStatus<uint64_t>
throw_bad_alloc(HostCallContext * /*unused*/)
{
  throw std::bad_alloc();
}

HostFnStatus<uint64_t>
good_call(HostCallContext * /*unused*/)
{
  return 10;
}

// Wasmi doesn't necessarily insert a "consume gas" instruction into its
// internal bytecode at the end of a call instruction. So if a host function
// uses all gas, it has to return kOutOfGas.
HostFnStatus<uint64_t>
consume_too_much_gas(HostCallContext *context)
{
  bool _ = context->runtime->consume_gas(context->runtime->get_available_gas());
  return HostFnError{HostFnErrorCode::kOutOfGas};
}

namespace {
WasmiRuntime *s_runtime = nullptr;
bool reentrance_hit = false;
} // namespace

HostFnStatus<uint64_t>
reentrance(HostCallContext * /*unused*/)
{
  reentrance_hit = true;

  auto res = s_runtime->invoke("unreachable", UINT64_MAX);
  if (!res.result.has_value()) {
    return HostFnError{HostFnErrorCode::kUnrecoverableUnknown};
  }
  return {0};
}

} // namespace test

class WasmiErrorHandlingTest : public ::testing::Test {
protected:
  void SetUp() override
  {
    contract_ = test::load_wasm_from_file("test_error_handling.wasm");
    context_.link_fn("test", "good_call", &test::good_call);
  }
  std::vector<uint8_t> contract_;
  WasmiContext context_;
  std::unique_ptr<WasmiRuntime> runtime_;
};

TEST_F(WasmiErrorHandlingTest, UnlinkedFn)
{
  runtime_ = context_.make_new_runtime(contract_, nullptr);
  EXPECT_FALSE(!!runtime_);
}

// make sure we don't break if errors do get thrown
TEST_F(WasmiErrorHandlingTest, RuntimeError)
{
  context_.link_fn("test", "external_call", &test::throw_runtime_error);
  runtime_ = context_.make_new_runtime(contract_, nullptr);

  auto res = runtime_->invoke("call1", UINT64_MAX);

  EXPECT_FALSE(!!res.result);
  EXPECT_EQ(res.result.error(),
            WasmiInvokeError{
                WasmiInvokeErrorCode::kWasmiUnrecoverableSystemError});
}

TEST_F(WasmiErrorHandlingTest, HostError)
{
  context_.link_fn("test", "external_call",
                   &test::throw_unrecoverable_host_error);
  runtime_ = context_.make_new_runtime(contract_, nullptr);

  auto res = runtime_->invoke("call1", UINT64_MAX);

  EXPECT_EQ(res.result.error(),
            WasmiInvokeError{
                WasmiInvokeErrorCode::kWasmiUnrecoverableSystemError});
}

TEST_F(WasmiErrorHandlingTest, OtherWeirdError)
{
  context_.link_fn("test", "external_call", &test::throw_bad_alloc);
  runtime_ = context_.make_new_runtime(contract_, nullptr);

  auto res = runtime_->invoke("call1", UINT64_MAX);

  EXPECT_FALSE(!!res.result);
  EXPECT_EQ(res.result.error(),
            WasmiInvokeError{
                WasmiInvokeErrorCode::kWasmiUnrecoverableSystemError});
}

TEST_F(WasmiErrorHandlingTest, WasmError)
{
  // must link all the fns, required for linking
  context_.link_fn("test", "external_call", &test::good_call);
  runtime_ = context_.make_new_runtime(contract_, nullptr);

  auto res = runtime_->invoke("call1", UINT64_MAX);

  EXPECT_TRUE(!!res.result);
  EXPECT_EQ(*res.result, 0);

  res = runtime_->invoke("unreachable", UINT64_MAX);

  EXPECT_FALSE(!!res.result);
  EXPECT_EQ(res.result.error(),
            WasmiInvokeError{
                WasmiInvokeErrorCode::kWasmiDeterministicCallError});
}

TEST_F(WasmiErrorHandlingTest, NonexistFn)
{
  // must link all the fns, required for linking
  context_.link_fn("test", "external_call", &test::good_call);
  runtime_ = context_.make_new_runtime(contract_, nullptr);

  auto res = runtime_->invoke("call1", UINT64_MAX);

  EXPECT_TRUE(!!res.result);

  res = runtime_->invoke("blah", UINT64_MAX);

  EXPECT_FALSE(!!res.result);
  EXPECT_EQ(res.result.error(),
            WasmiInvokeError{WasmiInvokeErrorCode::kWasmiFnNExist});
}

TEST_F(WasmiErrorHandlingTest, FollowBadWithGood)
{
  context_.link_fn("test", "external_call",
                   &test::throw_unrecoverable_host_error);
  runtime_ = context_.make_new_runtime(contract_, nullptr);

  auto res = runtime_->invoke("call1", UINT64_MAX);

  EXPECT_FALSE(!!res.result);
  EXPECT_EQ(res.result.error(),
            WasmiInvokeError{
                WasmiInvokeErrorCode::kWasmiUnrecoverableSystemError});

  res = runtime_->invoke("call2", UINT64_MAX);

  EXPECT_TRUE(!!res.result);
}

TEST_F(WasmiErrorHandlingTest, FollowReallyBadWithGood)
{
  context_.link_fn("test", "external_call", &test::throw_bad_alloc);
  runtime_ = context_.make_new_runtime(contract_, nullptr);

  auto res = runtime_->invoke("call1", UINT64_MAX);

  EXPECT_FALSE(!!res.result);
  EXPECT_EQ(res.result.error(),
            WasmiInvokeError{
                WasmiInvokeErrorCode::kWasmiUnrecoverableSystemError});

  res = runtime_->invoke("call2", UINT64_MAX);

  EXPECT_TRUE(!!res.result);
}

TEST_F(WasmiErrorHandlingTest, Reentrance)
{
  context_.link_fn("test", "external_call", &test::reentrance);
  runtime_ = context_.make_new_runtime(contract_, nullptr);

  test::s_runtime = runtime_.get();
  test::reentrance_hit = false;

  auto res = runtime_->invoke("call1", UINT64_MAX);

  EXPECT_FALSE(!!res.result);

  EXPECT_TRUE(test::reentrance_hit);
}

TEST_F(WasmiErrorHandlingTest, NullHandling)
{
  std::vector<uint8_t> null_contract;
  runtime_ = context_.make_new_runtime(null_contract, nullptr);

  EXPECT_FALSE(!!runtime_);
}

TEST_F(WasmiErrorHandlingTest, NullLinkHandling)
{
  // Apparently WASM accepts module/fn names of empty strings
  EXPECT_TRUE(context_.link_fn("", "good_call", &test::good_call));
  EXPECT_TRUE(context_.link_fn("test", "", &test::good_call));
  EXPECT_TRUE(context_.link_fn("foo", "good_call", &test::good_call));

  runtime_ = context_.make_new_runtime(contract_, nullptr);

  EXPECT_FALSE(!!runtime_);
}

TEST_F(WasmiErrorHandlingTest, OutOfGas)
{
  context_.link_fn("test", "external_call", &test::consume_too_much_gas);
  runtime_ = context_.make_new_runtime(contract_, nullptr);

  ASSERT_TRUE(!!runtime_);

  auto res = runtime_->invoke("call1", UINT64_MAX);

  EXPECT_FALSE(!!res.result);
  EXPECT_EQ(res.result.error(),
            WasmiInvokeError{WasmiInvokeErrorCode::kWasmiOutOfGasError});

  res = runtime_->invoke("call1", 1);

  EXPECT_FALSE(!!res.result);
  EXPECT_EQ(res.result.error(),
            WasmiInvokeError{WasmiInvokeErrorCode::kWasmiOutOfGasError});
}

TEST_F(WasmiErrorHandlingTest, NoGas)
{
  context_.link_fn("test", "external_call", &test::good_call);
  runtime_ = context_.make_new_runtime(contract_, nullptr);

  auto res = runtime_->invoke("call1", 1);

  EXPECT_FALSE(!!res.result);
  EXPECT_EQ(res.result.error(),
            WasmiInvokeError{WasmiInvokeErrorCode::kWasmiOutOfGasError});
}

TEST_F(WasmiErrorHandlingTest, Spin)
{
  context_.link_fn("test", "external_call", &test::good_call);
  runtime_ = context_.make_new_runtime(contract_, nullptr);

  auto res = runtime_->invoke("spin", 10000);

  EXPECT_FALSE(!!res.result);
  EXPECT_EQ(res.result.error(),
            WasmiInvokeError{WasmiInvokeErrorCode::kWasmiOutOfGasError});
}

} // namespace hogd
