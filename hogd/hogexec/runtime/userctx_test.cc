#include <cstdint>

#include <gtest/gtest.h>

#include "account.hh"
#include "contract.hh"
#include "hogd/base/types.h"
#include "hogd/crypto/crypto.h"
#include "hogd/hogexec/runtime/runtime.h"
#include "hogd/hogexec/runtime/status.h"
#include "hogd/hogexec/runtime/test_load_wasm.h"
#include "hogd/hogexec/runtime/wasm_test_framework.h"
#include "hogd/hogexec/runtime/wasmi_runtime.h"
#include "syscall_error.hh"

namespace hogd {

namespace {

SystemCallHandler *ctx_check = nullptr;

HostFnStatus<SyscallErrorCode>
foo(HostCallContext *ctx, uint64_t value)
{
  EXPECT_TRUE(ctx->syscall_handler == ctx_check);
  EXPECT_TRUE(value == 12);
  return SyscallErrorCode(15);
}

} // namespace

// NOLINTBEGIN(bugprone-unchecked-optional-access)
TEST(UserCtxTest, CheckCtxCorrect)
{
  WasmTestFramework framework;

  Contract contract(ContractType::kWasmi);
  contract.wasmi_contract() = test::load_wasm_from_file("test_invoke.wasm");

  framework.make_syscall_handler(contract);
  auto deployed_contract_hash = DeployedContractHash{
      .hash = sha256(contract),
      .contract_size =
          static_cast<uint32_t>(contract.wasmi_contract().wasm.size()),
      .contract_type = ContractType::kWasmi,
  };

  WasmiContext context;

  context.link_fn("test", "redir_call", &foo);

  auto runtime =
      context.make_new_runtime(deployed_contract_hash,
                               framework.syscall_handler_.get(), true);
  ASSERT_TRUE(runtime.has_value());
  ASSERT_TRUE(runtime.value() != nullptr);

  ctx_check = framework.syscall_handler_.get();

  auto res = runtime.value()->invoke("calltest", UINT64_MAX);
  EXPECT_TRUE(res.result_);
}
// NOLINTEND(bugprone-unchecked-optional-access)

} // namespace hogd
