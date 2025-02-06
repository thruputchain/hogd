#include <gtest/gtest.h>

#include <memory>
#include <utility>

#include "ffi_enums.hh"
#include "hogd/hogexec/runtime/mock_runtime.h"
#include "hogd/hogexec/runtime/mock_runtime_ffi.h"
#include "hogd/hogexec/runtime/runtime.h"
#include "types.hh"

namespace hogd {

namespace {
detail::ForeignInvokeResult
good_return()
{
  return detail::ForeignInvokeResult{.result_code = 32,
                                     .panic = std::to_underlying(
                                         InvokeErrorCode::kNone)};
}

} // namespace

class MockContractTest : public ::testing::Test {
protected:
  void SetUp() override
  {
    context_ = std::make_shared<MockRuntimeContext>(
        make_runtime_context(RuntimeEngine::kWasmi));
  }
  std::shared_ptr<MockRuntimeContext> context_;
};

TEST_F(MockContractTest, InvokeNormal)
{
  context_->add_mocked_method(SHA256Hash{}, "good", &good_return);

  auto runtime = context_->make_new_runtime(SHA256Hash{}, nullptr, nullptr);
  auto res = runtime->invoke("good", 100);
  ASSERT_TRUE(res.result_);
  EXPECT_EQ(*res.result_, 32);
}

} // namespace hogd
