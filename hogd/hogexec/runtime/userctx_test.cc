#include <cstdint>

#include <gtest/gtest.h>

#include "hogd/base/types.h"
#include "hogd/hogexec/runtime/status.h"
#include "hogd/hogexec/runtime/test_load_wasm.h"
#include "hogd/hogexec/runtime/wasmi_runtime.h"

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

TEST(WasmRuntimeTest, CheckCtxCorrect)
{
  auto c = test::load_wasm_from_file("test_invoke.wasm");
  WasmiContext context;

  context.link_fn("test", "redir_call", &foo);

  auto runtime = context.make_new_runtime(c, ctx_check);

  ASSERT_TRUE(!!runtime);

  auto res = runtime->invoke("calltest", UINT64_MAX);
  EXPECT_TRUE(!!res.result);
  EXPECT_EQ(*res.result, 15U);
}

} // namespace hogd
