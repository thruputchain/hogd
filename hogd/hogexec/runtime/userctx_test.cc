#include <cstdint>

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

SystemCallHandler *ctx_check =
    reinterpret_cast<SystemCallHandler *>(0xAABBCCDD'EEFF0011);

HostFnStatus<SyscallErrorCode>
foo(HostCallContext *ctx, uint64_t value)
{
  EXPECT_TRUE(ctx->syscall_handler == ctx_check);
  EXPECT_TRUE(value == 12);
  return SyscallErrorCode(15);
}

} // namespace

// NOLINTBEGIN(bugprone-unchecked-optional-access)
TEST(WasmRuntimeTest, CheckCtxCorrect)
{
  Contract contract(ContractType::kWasmi);
  contract.wasmi_contract() = test::load_wasm_from_file("test_invoke.wasm");
  WasmiContext context;

  context.link_fn("test", "redir_call", &foo);

  auto runtime = context.make_new_runtime(contract, ctx_check, true);

  ASSERT_TRUE(runtime);

  auto res = runtime->invoke("calltest", UINT64_MAX);
  EXPECT_TRUE(res.result_);
}
// NOLINTEND(bugprone-unchecked-optional-access)

} // namespace hogd
