#include <cstdint>
#include <memory>

#include <gtest/gtest.h>

#include "ffi_enums.hh"
#include "hogd/base/types.h"
#include "hogd/hogexec/runtime/runtime.h"
#include "hogd/hogexec/runtime/status.h"
#include "hogd/hogexec/runtime/test_load_wasm.h"

namespace hogd {

namespace {

SystemCallHandler *ctx_check =
    reinterpret_cast<SystemCallHandler *>(0xAABBCCDD'EEFF0011);

HostFnResult
foo(HostCallContext *ctx, uint64_t value)
{
  EXPECT_TRUE(ctx->syscall_handler == ctx_check);
  EXPECT_TRUE(value == 12);
  return {HostFnErrorCode::kNoneOrRecoverable, 15};
}

} // namespace

class UserContextTest : public ::testing::TestWithParam<RuntimeEngine> {
protected:
  void SetUp() override
  {
    auto c = Contract{.contract_data =
                          test::load_wasm_from_file("test_invoke.wasm")};
    auto context = make_runtime_context(GetParam());
    context->link_fn("test", "redir_call", &foo);
    runtime_ = context->make_new_runtime(SHA256Hash{}, &c, ctx_check);

    ASSERT_TRUE(runtime_);
  }

  std::unique_ptr<Runtime> runtime_;
};

TEST_P(UserContextTest, CheckCtxCorrect)
{
  auto res = runtime_->invoke("calltest", UINT64_MAX);
  EXPECT_TRUE(res.result_);
  EXPECT_EQ(*res.result_, 15U);
}

INSTANTIATE_TEST_SUITE_P(AllEngines, UserContextTest,
                         ::testing::Values(RuntimeEngine::kWasmi,
                                           RuntimeEngine::kWasm3),
                         [](const testing::TestParamInfo<RuntimeEngine> &info) {
                           return engine_to_string(info.param);
                         });

} // namespace hogd
