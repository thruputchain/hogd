#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <new>
#include <stdexcept>

#include "contract.hh"
#include "ffi_enums.hh"
#include "hogd/base/types.h"
#include "hogd/hogexec/runtime/runtime.h"
#include "hogd/hogexec/runtime/status.h"
#include "hogd/hogexec/runtime/test_load_wasm.h"

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
  return {HostFnErrorCode::kHostUnrecoverable, 0};
}

HostFnResult
throw_validation_error(HostCallContext * /*unused*/)
{
  return {HostFnErrorCode::kHostValidationFailure, 0};
}

HostFnResult
throw_bad_alloc(HostCallContext * /*unused*/)
{
  throw std::bad_alloc();
}

HostFnResult
good_call(HostCallContext * /*unused*/)
{
  return {HostFnErrorCode::kHostNoneOrRecoverable, 10};
}

HostFnResult
exit_call(HostCallContext * /*unused*/)
{
  return {HostFnErrorCode::kHostExit, 10};
}

HostFnResult
return_syscall(HostCallContext * /*unused*/)
{
  return {HostFnErrorCode::kHostReturn, 0};
}

// Wasmi doesn't necessarily insert a "consume gas" instruction into its
// internal bytecode at the end of a call instruction. So if a host function
// uses all gas, it has to return kOutOfGas.
HostFnResult
consume_too_much_gas(HostCallContext *context)
{
  bool _ = context->runtime->consume_gas(context->runtime->get_available_gas());
  return {HostFnErrorCode::kHostOutOfGas, 0};
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
  if (!res.result_.has_value()) {
    return {HostFnErrorCode::kHostUnrecoverable, 0};
  }
  return {HostFnErrorCode::kHostNoneOrRecoverable, 0};
}

} // namespace

class ErrorHandlingTest : public ::testing::TestWithParam<RuntimeEngine> {
protected:
  void SetUp() override
  {
    contract_.contract_type(ContractType::kWasmi);
    contract_.wasmi_contract() =
        test::load_wasm_from_file("test_error_handling.wasm");
    context_->link_fn("test", "good_call", &good_call);
  }
  Contract contract_;
  std::shared_ptr<RuntimeContext> context_ = make_runtime_context(GetParam());
  std::unique_ptr<Runtime> runtime_;
};

TEST_P(ErrorHandlingTest, UnlinkedFn)
{
  runtime_ = context_->make_new_runtime(contract_, nullptr, true);

  EXPECT_EQ(!!runtime_, !engine_has_eager_link_guards(GetParam()));
}

// make sure we don't break if errors do get thrown
TEST_P(ErrorHandlingTest, RuntimeError)
{
  context_->link_fn("test", "external_call", &throw_runtime_error);
  runtime_ = context_->make_new_runtime(contract_, nullptr, true);

  ASSERT_TRUE(runtime_);

  auto res = runtime_->invoke("call1", UINT64_MAX);

  EXPECT_FALSE(res.result_);
  EXPECT_EQ(res.result_.error(),
            InvokeError{InvokeErrorCode::kInvokeUnrecoverable});
}

TEST_P(ErrorHandlingTest, ExitHandling)
{
  context_->link_fn("test", "external_call", &exit_call);
  runtime_ = context_->make_new_runtime(contract_, nullptr, true);

  ASSERT_TRUE(runtime_);

  auto res = runtime_->invoke("call1", UINT64_MAX);

  EXPECT_TRUE(res.result_);
  EXPECT_EQ(*res.result_, 10);
}

TEST_P(ErrorHandlingTest, HostError)
{
  context_->link_fn("test", "external_call", &throw_unrecoverable_host_error);
  runtime_ = context_->make_new_runtime(contract_, nullptr, true);

  ASSERT_TRUE(!!runtime_);

  auto res = runtime_->invoke("call1", UINT64_MAX);

  EXPECT_EQ(res.result_.error(),
            InvokeError{InvokeErrorCode::kInvokeUnrecoverable});
}

TEST_P(ErrorHandlingTest, ValidationError)
{
  context_->link_fn("test", "external_call", &throw_validation_error);
  runtime_ = context_->make_new_runtime(contract_, nullptr, true);

  ASSERT_TRUE(!!runtime_);

  auto res = runtime_->invoke("call1", UINT64_MAX);

  EXPECT_EQ(res.result_.error(),
            InvokeError{InvokeErrorCode::kInvokeValidationFailure});
}

TEST_P(ErrorHandlingTest, OtherWeirdError)
{
  context_->link_fn("test", "external_call", &throw_bad_alloc);
  runtime_ = context_->make_new_runtime(contract_, nullptr, true);

  ASSERT_TRUE(runtime_);

  auto res = runtime_->invoke("call1", UINT64_MAX);

  EXPECT_FALSE(res.result_);
  EXPECT_EQ(res.result_.error(),
            InvokeError{InvokeErrorCode::kInvokeUnrecoverable});
}

TEST_P(ErrorHandlingTest, WasmError)
{
  // must link all the fns, required for linking
  context_->link_fn("test", "external_call", &good_call);
  runtime_ = context_->make_new_runtime(contract_, nullptr, true);

  ASSERT_TRUE(runtime_);

  auto res = runtime_->invoke("call1", UINT64_MAX);

  EXPECT_TRUE(res.result_);
  EXPECT_EQ(*res.result_, 0);

  res = runtime_->invoke("unreachable", UINT64_MAX);

  EXPECT_FALSE(res.result_);
  EXPECT_EQ(res.result_.error(),
            InvokeError{InvokeErrorCode::kInvokeDeterministic});
}

TEST_P(ErrorHandlingTest, NonexistFn)
{
  // must link all the fns, required for linking
  context_->link_fn("test", "external_call", &good_call);
  runtime_ = context_->make_new_runtime(contract_, nullptr, true);

  ASSERT_TRUE(runtime_);

  auto res = runtime_->invoke("call1", UINT64_MAX);

  EXPECT_TRUE(res.result_);

  res = runtime_->invoke("blah", UINT64_MAX);

  EXPECT_FALSE(res.result_);
  EXPECT_EQ(res.result_.error(),
            InvokeError{InvokeErrorCode::kInvokeDeterministic});
}

TEST_P(ErrorHandlingTest, FollowBadWithGood)
{
  context_->link_fn("test", "external_call", &throw_unrecoverable_host_error);
  runtime_ = context_->make_new_runtime(contract_, nullptr, true);

  ASSERT_TRUE(runtime_);

  auto res = runtime_->invoke("call1", UINT64_MAX);

  EXPECT_FALSE(res.result_);
  EXPECT_EQ(res.result_.error(),
            InvokeError{InvokeErrorCode::kInvokeUnrecoverable});

  res = runtime_->invoke("call2", UINT64_MAX);

  EXPECT_TRUE(res.result_);
}

TEST_P(ErrorHandlingTest, CheckReturn)
{
  context_->link_fn("test", "external_call", &return_syscall);
  runtime_ = context_->make_new_runtime(contract_, nullptr, true);

  ASSERT_TRUE(runtime_);

  auto res = runtime_->invoke("call_then_unreachable", UINT64_MAX);

  EXPECT_TRUE(res.result_);
}

TEST_P(ErrorHandlingTest, FollowReallyBadWithGood)
{
  context_->link_fn("test", "external_call", &throw_bad_alloc);
  runtime_ = context_->make_new_runtime(contract_, nullptr, true);

  ASSERT_TRUE(runtime_);

  auto res = runtime_->invoke("call1", UINT64_MAX);

  EXPECT_FALSE(res.result_);
  EXPECT_EQ(res.result_.error(),
            InvokeError{InvokeErrorCode::kInvokeUnrecoverable});

  res = runtime_->invoke("call2", UINT64_MAX);

  EXPECT_TRUE(res.result_);
}

TEST_P(ErrorHandlingTest, Reentrance)
{
  context_->link_fn("test", "external_call", &reentrance);
  runtime_ = context_->make_new_runtime(contract_, nullptr, true);

  ASSERT_TRUE(runtime_);

  s_runtime = runtime_.get();
  reentrance_hit = false;

  auto res = runtime_->invoke("call1", UINT64_MAX);

  EXPECT_FALSE(res.result_);

  EXPECT_TRUE(reentrance_hit);
}

TEST_P(ErrorHandlingTest, NullHandling)
{
  Contract null_contract(ContractType::kWasmi);
  runtime_ = context_->make_new_runtime(null_contract, nullptr, true);

  EXPECT_FALSE(runtime_);
}

TEST_P(ErrorHandlingTest, NullLinkHandling)
{
  // Apparently WASM accepts module/fn names of empty strings
  // We guard against that because that behavior is weird
  EXPECT_FALSE(context_->link_fn("", "good_call", &good_call));
  EXPECT_FALSE(context_->link_fn("test", "", &good_call));
  EXPECT_TRUE(context_->link_fn("foo", "good_call", &good_call));
  EXPECT_FALSE(context_->link_fn(
      "foo", "bar",
      static_cast<HostFnResult (*)(HostCallContext *, uint64_t)>(nullptr)));
}

TEST_P(ErrorHandlingTest, OutOfGas)
{
  context_->link_fn("test", "external_call", &consume_too_much_gas);
  runtime_ = context_->make_new_runtime(contract_, nullptr, true);

  ASSERT_TRUE(runtime_);

  auto res = runtime_->invoke("call1", UINT64_MAX);

  EXPECT_FALSE(res.result_);
  EXPECT_EQ(res.result_.error(), InvokeError{InvokeErrorCode::kInvokeOutOfGas});

  res = runtime_->invoke("call1", 1);

  EXPECT_FALSE(res.result_);
  EXPECT_EQ(res.result_.error(), InvokeError{InvokeErrorCode::kInvokeOutOfGas});
}

TEST_P(ErrorHandlingTest, NoGas)
{
  context_->link_fn("test", "external_call", &good_call);
  runtime_ = context_->make_new_runtime(contract_, nullptr, true);

  ASSERT_TRUE(runtime_);

  auto res = runtime_->invoke("call1", 1);

  if (!engine_has_native_metering(GetParam()))
    return;

  EXPECT_FALSE(res.result_);
  EXPECT_EQ(res.result_.error(), InvokeError{InvokeErrorCode::kInvokeOutOfGas});
}

TEST_P(ErrorHandlingTest, Spin)
{
  context_->link_fn("test", "external_call", &good_call);
  runtime_ = context_->make_new_runtime(contract_, nullptr, true);

  ASSERT_TRUE(runtime_);

  if (!engine_has_native_metering(GetParam()))
    return;

  auto res = runtime_->invoke("spin", 10000);

  EXPECT_FALSE(res.result_);
  EXPECT_EQ(res.result_.error(), InvokeError{InvokeErrorCode::kInvokeOutOfGas});
}

INSTANTIATE_TEST_SUITE_P(AllEngines, ErrorHandlingTest,
                         ::testing::Values(RuntimeEngine::kWasmi,
                                           RuntimeEngine::kWasm3),
                         [](const testing::TestParamInfo<RuntimeEngine> &info) {
                           return engine_to_string(info.param);
                         });

} // namespace hogd
