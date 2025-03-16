#include <cstddef>
#include <cstdint>
#include <cstring>
#include <gtest/gtest.h>

#include <memory>
#include <span>
#include <tuple>
#include <utility>
#include <vector>

#include "ffi_enums.hh"
#include "hogd/base/types.h"
#include "hogd/hogexec/runtime/mock_runtime.h"
#include "hogd/hogexec/runtime/runtime.h"
#include "hogd/hogexec/runtime/status.h"
#include "hogd/test_ffi/test_bindings.h"
#include "syscall.hh"
#include "types.hh"
#include "x23/vec.h"
#include "x23/x23.h"

namespace hogd {

namespace {

void *c_expect = reinterpret_cast<void *>(0x1234);

ForeignInvokeResult
good_return(void *context, MethodID method_id)
{
  EXPECT_EQ(context, c_expect);
  return ForeignInvokeResult{.result_code = static_cast<int32_t>(method_id),
                             .panic =
                                 std::to_underlying(InvokeErrorCode::kNone)};
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
  context_->set_trampoline(SHA256Hash{}, &good_return, c_expect);

  auto runtime = context_->make_new_runtime(SHA256Hash{}, nullptr, nullptr);
  auto res = runtime->invoke("32", 100);
  ASSERT_TRUE(res.result_);
  EXPECT_EQ(*res.result_, 50);

  res = runtime->invoke("Invalid", 10);
  ASSERT_FALSE(res.result_);
  EXPECT_EQ(res.result_.error().code(), InvokeErrorCode::kUnrecoverable);
}

TEST_F(MockContractTest, MethodInversion)
{
  auto check_roundtrip = [](MethodID id) {
    EXPECT_EQ(id,
              Runtime::method_id_from_name(Runtime::method_name_from_int(id)));
  };

  check_roundtrip(0);
  check_roundtrip(UINT32_MAX);
  check_roundtrip(0xABCDEF01);
}

} // namespace hogd
