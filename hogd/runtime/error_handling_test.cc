#include <gtest/gtest.h>

#include "hogd/base/types.h"
#include "hogd/runtime/host_fn_status.h"
#include "hogd/runtime/test_load_wasm.h"
#include "hogd/runtime/wasmi_runtime.h"

#include <cstdint>
#include <memory>
#include <stdexcept>
#include <vector>

namespace hogd {

namespace test {

HostFnStatus<uint64_t>
throw_runtime_error(HostCallContext *)
{
  throw std::runtime_error("bad fn\n");
}

HostFnStatus<uint64_t>
throw_host_error(HostCallContext *)
{
  return HostFnError{HostFnErrorCode::kRecoverableUnknown};
}

HostFnStatus<uint64_t>
throw_bad_alloc(HostCallContext *)
{
  throw std::bad_alloc();
}

HostFnStatus<uint64_t>
good_call(HostCallContext *)
{
  return 0;
}

namespace {
WasmiRuntime *s_runtime = nullptr;
bool reentrance_hit = false;
} // namespace

HostFnStatus<uint64_t>
reentrance(HostCallContext *)
{
  reentrance_hit = true;

  auto res = s_runtime->invoke("unreachable", UINT64_MAX);
  return res.result; // rethrow
}

} // namespace test

class WasmiErrorHandlingTest : public ::testing::Test {
protected:
  void SetUp() override
  {
    auto c = test::load_wasm_from_file("test_error_handling.wasm");
    runtime_ = std::make_unique<WasmiRuntime>(c, nullptr);
    runtime_->template link_fn<&test::good_call>("test", "good_call");
  }

  std::unique_ptr<WasmiRuntime> runtime_;
};

TEST_F(WasmiErrorHandlingTest, UnlinkedFn)
{
  auto res = runtime_->invoke("call1", UINT64_MAX);
  EXPECT_FALSE(!!res.result);
  EXPECT_EQ(res.result.error(), HostFnError{HostFnErrorCode::kWasmiLinkError});
}

// make sure we don't break if errors do get thrown
TEST_F(WasmiErrorHandlingTest, RuntimeError)
{
  runtime_->template link_fn<&test::throw_runtime_error>("test",
                                                         "external_call");
  auto res = runtime_->invoke("call1", UINT64_MAX);

  EXPECT_FALSE(!!res.result);
  EXPECT_EQ(res.result.error(),
            HostFnError{HostFnErrorCode::kWasmiUnrecoverableSystemError});
}

TEST_F(WasmiErrorHandlingTest, HostError)
{
  runtime_->template link_fn<&test::throw_host_error>("test", "external_call");

  auto res = runtime_->invoke("call1", UINT64_MAX);

  EXPECT_EQ(res.result.error(), HostFnError{HostFnErrorCode::kWasmiHostError});
}

TEST_F(WasmiErrorHandlingTest, OtherWeirdError)
{
  runtime_->template link_fn<&test::throw_bad_alloc>("test", "external_call");

  auto res = runtime_->invoke("call1", UINT64_MAX);

  EXPECT_FALSE(!!res.result);
  EXPECT_EQ(res.result.error(),
            HostFnError{HostFnErrorCode::kWasmiUnrecoverableSystemError});
}

TEST_F(WasmiErrorHandlingTest, WasmError)
{
  // must link all the fns, required for linking
  runtime_->template link_fn<&test::good_call>("test", "external_call");

  auto res = runtime_->invoke("call1", UINT64_MAX);

  EXPECT_TRUE(!!res.result);

  res = runtime_->invoke("unreachable", UINT64_MAX);

  EXPECT_FALSE(!!res.result);
  EXPECT_EQ(res.result.error(), HostFnError{HostFnErrorCode::kWasmiCallError});
}

TEST_F(WasmiErrorHandlingTest, NonexistFn)
{
  // must link all the fns, required for linking
  runtime_->template link_fn<&test::good_call>("test", "external_call");

  auto res = runtime_->invoke("call1", UINT64_MAX);

  EXPECT_TRUE(!!res.result);

  res = runtime_->invoke("blah", UINT64_MAX);

  EXPECT_FALSE(!!res.result);
  EXPECT_EQ(res.result.error(), HostFnError{HostFnErrorCode::kWasmiFnNExist});
}

TEST_F(WasmiErrorHandlingTest, FollowBadWithGood)
{
  runtime_->template link_fn<&test::throw_host_error>("test", "external_call");

  auto res = runtime_->invoke("call1", UINT64_MAX);

  EXPECT_FALSE(!!res.result);
  EXPECT_EQ(res.result.error(), HostFnError{HostFnErrorCode::kWasmiHostError});

  res = runtime_->invoke("call2", UINT64_MAX);

  EXPECT_TRUE(!!res.result);
}

TEST_F(WasmiErrorHandlingTest, FollowReallyBadWithGood)
{
  runtime_->template link_fn<&test::throw_bad_alloc>("test", "external_call");

  auto res = runtime_->invoke("call1", UINT64_MAX);

  EXPECT_FALSE(!!res.result);
  EXPECT_EQ(res.result.error(),
            HostFnError{HostFnErrorCode::kWasmiUnrecoverableSystemError});

  res = runtime_->invoke("call2", UINT64_MAX);

  EXPECT_TRUE(!!res.result);
}

TEST_F(WasmiErrorHandlingTest, Reentrance)
{
  runtime_->link_fn<&test::reentrance>("test", "external_call");
  test::s_runtime = runtime_.get();
  test::reentrance_hit = false;

  auto res = runtime_->invoke("call1", UINT64_MAX);

  EXPECT_FALSE(!!res.result);

  EXPECT_TRUE(test::reentrance_hit);
}

TEST_F(WasmiErrorHandlingTest, NullHandling)
{
  std::vector<uint8_t> null_contract;
  runtime_ = std::make_unique<WasmiRuntime>(null_contract, nullptr);

  // linker would require linking all fns before invoke worked anyways
  runtime_->template link_fn<&test::good_call>("test", "good_call");
  runtime_->template link_fn<&test::good_call>("test", "external_call");

  auto res = runtime_->invoke("bad", UINT64_MAX);

  EXPECT_FALSE(!!res.result);

  res = runtime_->invoke("", UINT64_MAX);

  EXPECT_FALSE(!!res.result);
}

TEST_F(WasmiErrorHandlingTest, NullLinkHandling)
{
  std::vector<uint8_t> null_contract;
  runtime_ = std::make_unique<WasmiRuntime>(null_contract, nullptr);

  // linker would require linking all fns before invoke worked anyways
  runtime_->template link_fn<&test::good_call>("", "good_call");
  runtime_->template link_fn<&test::good_call>("test", "");

  auto res = runtime_->invoke("call1", UINT64_MAX);

  EXPECT_FALSE(!!res.result);
}

} // namespace hogd
