#include <gtest/gtest.h>

#include "hogd/base/types.h"
#include "hogd/runtime/status.h"
#include "hogd/runtime/test_load_wasm.h"
#include "hogd/runtime/wasmi_runtime.h"
#include <cstdint>

namespace hogd {

SystemCallHandler *ctx_check =
    reinterpret_cast<SystemCallHandler *>(0xAABBCCDD'EEFF0011);

HostFnStatus<uint64_t>
foo(HostCallContext *ctx, uint64_t value)
{
  EXPECT_TRUE(ctx->syscall_handler == ctx_check);
  EXPECT_TRUE(value == 12);
  return 15;
}

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
