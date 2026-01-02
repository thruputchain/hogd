#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <new>
#include <stdexcept>

#include "account.hh"
#include "contract.hh"
#include "ffi_enums.hh"
#include "hogd/base/types.h"
#include "hogd/crypto/crypto.h"
#include "hogd/hogexec/runtime/runtime.h"
#include "hogd/hogexec/runtime/status.h"
#include "hogd/hogexec/runtime/test_load_wasm.h"
#include "hogd/hogexec/runtime/wasm_test_framework.h"
#include "syscall_error.hh"

namespace hogd {

namespace {

HostFnStatus<SyscallErrorCode>
throw_runtime_error(HostCallContext * /*unused*/)
{
  throw std::runtime_error("bad fn\n");
}

HostFnStatus<SyscallErrorCode>
throw_unrecoverable_host_error(HostCallContext * /*unused*/)
{
  return HostFnError{HostFnErrorCode::kHostUnrecoverable};
}

HostFnStatus<SyscallErrorCode>
throw_validation_error(HostCallContext * /*unused*/)
{
  return HostFnError{HostFnErrorCode::kHostValidationFailure};
}

HostFnStatus<SyscallErrorCode>
throw_abort_transaction(HostCallContext * /*unused*/)
{
  return HostFnError{HostFnErrorCode::kHostAbortTransaction};
}

HostFnStatus<SyscallErrorCode>
throw_bad_alloc(HostCallContext * /*unused*/)
{
  throw std::bad_alloc();
}

HostFnStatus<SyscallErrorCode>
good_call(HostCallContext * /*unused*/)
{
  return SyscallErrorCode::kSyscallErrorCodeNone;
}

HostFnStatus<SyscallErrorCode>
exit_call(HostCallContext * /*unused*/)
{
  return HostFnError{HostFnErrorCode::kHostExit};
}

HostFnStatus<SyscallErrorCode>
return_syscall(HostCallContext * /*unused*/)
{
  return HostFnError{HostFnErrorCode::kHostReturn};
}

// Wasmi doesn't necessarily insert a "consume gas" instruction into its
// internal bytecode at the end of a call instruction. So if a host function
// uses all gas, it has to return kOutOfGas.
HostFnStatus<SyscallErrorCode>
consume_too_much_gas(HostCallContext *context)
{
  bool _ = context->runtime->consume_gas(context->runtime->get_available_gas());
  return HostFnError{HostFnErrorCode::kHostOutOfGas};
}

namespace {
Runtime *s_runtime = nullptr;
bool reentrance_hit = false;
} // namespace

HostFnStatus<SyscallErrorCode>
reentrance(HostCallContext * /*unused*/)
{
  reentrance_hit = true;
  if (s_runtime == nullptr) {
    return HostFnError{HostFnErrorCode::kHostUnrecoverable};
  }

  auto res = s_runtime->invoke("unreachable", UINT64_MAX);
  if (!res.result_.has_value()) {
    return HostFnError{HostFnErrorCode::kHostUnrecoverable};
  }
  return SyscallErrorCode::kSyscallErrorCodeNone;
}

} // namespace

class WasmiErrorHandlingTest : public ::testing::TestWithParam<WasmEngine>,
                               public WasmTestFramework {
protected:
  void SetUp() override
  {
    contract_.contract_type(ContractType::kWasmi);
    contract_.wasmi_contract() =
        test::load_wasm_from_file("test_error_handling.wasm");

    deployed_contract_hash_ = DeployedContractHash{
        .hash = sha256(contract_),
        .contract_size =
            static_cast<uint32_t>(contract_.wasmi_contract().wasm.size()),
        .contract_type = ContractType::kWasmi,
    };

    make_syscall_handler(contract_);
    setup_wasm_context(GetParam());
    link_fn("test", "good_call", &good_call);
  }

  Contract contract_;
  DeployedContractHash deployed_contract_hash_;
};

TEST_P(WasmiErrorHandlingTest, UnlinkedFn)
{
  auto runtime = get_context()->make_new_runtime(deployed_contract_hash_,
                                                 syscall_handler_.get(), true);
  ASSERT_TRUE(runtime.has_value());
  EXPECT_TRUE(runtime.value() == nullptr);
}

// make sure we don't break if errors do get thrown
TEST_P(WasmiErrorHandlingTest, RuntimeError)
{
  link_fn("test", "external_call", &throw_runtime_error);
  auto runtime = get_context()->make_new_runtime(deployed_contract_hash_,
                                                 syscall_handler_.get(), true);
  ASSERT_TRUE(runtime.has_value());
  ASSERT_TRUE(runtime.value() != nullptr);

  auto res = runtime.value()->invoke("call1", UINT64_MAX);

  EXPECT_FALSE(res.result_);
  EXPECT_EQ(res.result_.error(),
            InvokeError{InvokeErrorCode::kInvokeUnrecoverable});
}

TEST_P(WasmiErrorHandlingTest, ExitHandling)
{
  link_fn("test", "external_call", &exit_call);
  auto runtime = get_context()->make_new_runtime(deployed_contract_hash_,
                                                 syscall_handler_.get(), true);

  ASSERT_TRUE(runtime.has_value());
  ASSERT_TRUE(runtime.value() != nullptr);

  auto res = runtime.value()->invoke("call1", UINT64_MAX);

  EXPECT_FALSE(res.is_root_success());
}

TEST_P(WasmiErrorHandlingTest, HostError)
{
  link_fn("test", "external_call", &throw_unrecoverable_host_error);
  auto runtime = get_context()->make_new_runtime(deployed_contract_hash_,
                                                 syscall_handler_.get(), true);

  ASSERT_TRUE(runtime.has_value());
  ASSERT_TRUE(runtime.value() != nullptr);

  auto res = runtime.value()->invoke("call1", UINT64_MAX);
  EXPECT_EQ(res.result_.error(),
            InvokeError{InvokeErrorCode::kInvokeUnrecoverable});
}

TEST_P(WasmiErrorHandlingTest, AbortTransaction)
{
  link_fn("test", "external_call", &throw_abort_transaction);
  auto runtime = get_context()->make_new_runtime(deployed_contract_hash_,
                                                 syscall_handler_.get(), true);

  ASSERT_TRUE(runtime.has_value());
  ASSERT_TRUE(runtime.value() != nullptr);

  auto res = runtime.value()->invoke("call1", UINT64_MAX);
  EXPECT_EQ(res.result_.error(),
            InvokeError{InvokeErrorCode::kInvokeAbortTransaction});
}

TEST_P(WasmiErrorHandlingTest, ValidationError)
{
  link_fn("test", "external_call", &throw_validation_error);
  auto runtime = get_context()->make_new_runtime(deployed_contract_hash_,
                                                 syscall_handler_.get(), true);

  ASSERT_TRUE(runtime.has_value());
  ASSERT_TRUE(runtime.value() != nullptr);

  auto res = runtime.value()->invoke("call1", UINT64_MAX);

  EXPECT_EQ(res.result_.error(),
            InvokeError{InvokeErrorCode::kInvokeValidationFailure});
}

TEST_P(WasmiErrorHandlingTest, OtherWeirdError)
{
  link_fn("test", "external_call", &throw_bad_alloc);
  auto runtime = get_context()->make_new_runtime(deployed_contract_hash_,
                                                 syscall_handler_.get(), true);

  ASSERT_TRUE(runtime.has_value());
  ASSERT_TRUE(runtime.value() != nullptr);

  auto res = runtime.value()->invoke("call1", UINT64_MAX);

  EXPECT_FALSE(res.result_);
  EXPECT_EQ(res.result_.error(),
            InvokeError{InvokeErrorCode::kInvokeUnrecoverable});
}

TEST_P(WasmiErrorHandlingTest, WasmError)
{
  // must link all the fns, required for linking
  link_fn("test", "external_call", &good_call);
  auto runtime = get_context()->make_new_runtime(deployed_contract_hash_,
                                                 syscall_handler_.get(), true);

  ASSERT_TRUE(runtime.has_value());
  ASSERT_TRUE(runtime.value() != nullptr);

  auto res = runtime.value()->invoke("call1", UINT64_MAX);

  EXPECT_TRUE(res.result_);

  res = runtime.value()->invoke("unreachable", UINT64_MAX);

  EXPECT_FALSE(res.result_);
  EXPECT_EQ(res.result_.error(),
            InvokeError{InvokeErrorCode::kInvokeDeterministic});
}

TEST_P(WasmiErrorHandlingTest, NonexistFn)
{
  // must link all the fns, required for linking
  link_fn("test", "external_call", &good_call);
  auto runtime = get_context()->make_new_runtime(deployed_contract_hash_,
                                                 syscall_handler_.get(), true);

  ASSERT_TRUE(runtime.has_value());
  ASSERT_TRUE(runtime.value() != nullptr);

  auto res = runtime.value()->invoke("call1", UINT64_MAX);

  EXPECT_TRUE(res.result_);

  res = runtime.value()->invoke("blah", UINT64_MAX);

  EXPECT_FALSE(res.result_);
  EXPECT_EQ(res.result_.error(),
            InvokeError{InvokeErrorCode::kInvokeDeterministic});
}

TEST_P(WasmiErrorHandlingTest, FollowBadWithGood)
{
  link_fn("test", "external_call", &throw_unrecoverable_host_error);
  auto runtime = get_context()->make_new_runtime(deployed_contract_hash_,
                                                 syscall_handler_.get(), true);

  ASSERT_TRUE(runtime.has_value());
  ASSERT_TRUE(runtime.value() != nullptr);

  auto res = runtime.value()->invoke("call1", UINT64_MAX);

  EXPECT_FALSE(res.result_);
  EXPECT_EQ(res.result_.error(),
            InvokeError{InvokeErrorCode::kInvokeUnrecoverable});

  res = runtime.value()->invoke("call2", UINT64_MAX);

  EXPECT_TRUE(res.result_);
}

TEST_P(WasmiErrorHandlingTest, CheckReturn)
{
  link_fn("test", "external_call", &return_syscall);
  auto runtime = get_context()->make_new_runtime(deployed_contract_hash_,
                                                 syscall_handler_.get(), true);

  ASSERT_TRUE(runtime.has_value());
  ASSERT_TRUE(runtime.value() != nullptr);

  auto res = runtime.value()->invoke("call_then_unreachable", UINT64_MAX);

  EXPECT_TRUE(res.result_);
}

TEST_P(WasmiErrorHandlingTest, FollowReallyBadWithGood)
{
  link_fn("test", "external_call", &throw_bad_alloc);
  auto runtime = get_context()->make_new_runtime(deployed_contract_hash_,
                                                 syscall_handler_.get(), true);

  ASSERT_TRUE(runtime.has_value());
  ASSERT_TRUE(runtime.value() != nullptr);

  auto res = runtime.value()->invoke("call1", UINT64_MAX);

  EXPECT_FALSE(res.result_);
  EXPECT_EQ(res.result_.error(),
            InvokeError{InvokeErrorCode::kInvokeUnrecoverable});

  res = runtime.value()->invoke("call2", UINT64_MAX);

  EXPECT_TRUE(res.result_);
}

TEST_P(WasmiErrorHandlingTest, Reentrance)
{
  link_fn("test", "external_call", &reentrance);
  auto runtime = get_context()->make_new_runtime(deployed_contract_hash_,
                                                 syscall_handler_.get(), true);

  ASSERT_TRUE(runtime.has_value());
  ASSERT_TRUE(runtime.value() != nullptr);

  s_runtime = runtime.value().get();
  reentrance_hit = false;

  auto res = runtime.value()->invoke("call1", UINT64_MAX);

  EXPECT_FALSE(res.result_);

  EXPECT_TRUE(reentrance_hit);
}

TEST_P(WasmiErrorHandlingTest, NullHandling)
{
  Contract null_contract(ContractType::kWasmi);
  make_syscall_handler(null_contract);
  auto null_deployed_contract_hash = DeployedContractHash{
      .hash = sha256(null_contract),
      .contract_size = 0,
      .contract_type = ContractType::kWasmi,
  };
  auto runtime = get_context()->make_new_runtime(null_deployed_contract_hash,
                                                 syscall_handler_.get(), true);
  ASSERT_TRUE(runtime.has_value());

  EXPECT_FALSE(runtime.value());
}

TEST_P(WasmiErrorHandlingTest, NullLinkHandling)
{
  // The WASM standard accepts empty strings, but we ban that out to keep the
  // ffi handling simpler (meaning we never have a reason to pass a null string
  // across FFI).
  EXPECT_FALSE(link_fn("", "good_call", &good_call));
  EXPECT_FALSE(link_fn("test", "", &good_call));
  EXPECT_TRUE(link_fn("foo", "good_call", &good_call));

  auto runtime = get_context()->make_new_runtime(deployed_contract_hash_,
                                                 syscall_handler_.get(), true);
  ASSERT_TRUE(runtime.has_value());

  EXPECT_FALSE(runtime.value());
}

TEST_P(WasmiErrorHandlingTest, OutOfGas)
{
  link_fn("test", "external_call", &consume_too_much_gas);
  auto runtime = get_context()->make_new_runtime(deployed_contract_hash_,
                                                 syscall_handler_.get(), true);

  ASSERT_TRUE(runtime.has_value());
  ASSERT_TRUE(runtime.value() != nullptr);

  auto res = runtime.value()->invoke("call1", UINT64_MAX);

  EXPECT_FALSE(res.result_);
  EXPECT_EQ(res.result_.error(), InvokeError{InvokeErrorCode::kInvokeOutOfGas});

  res = runtime.value()->invoke("call1", 1);

  EXPECT_FALSE(res.result_);
  EXPECT_EQ(res.result_.error(), InvokeError{InvokeErrorCode::kInvokeOutOfGas});
}

TEST_P(WasmiErrorHandlingTest, NoGas)
{
  link_fn("test", "external_call", &good_call);
  auto runtime = get_context()->make_new_runtime(deployed_contract_hash_,
                                                 syscall_handler_.get(), true);

  ASSERT_TRUE(runtime.has_value());
  ASSERT_TRUE(runtime.value() != nullptr);

  auto res = runtime.value()->invoke("call1", 1);

  EXPECT_FALSE(res.result_);
  EXPECT_EQ(res.result_.error(), InvokeError{InvokeErrorCode::kInvokeOutOfGas});
}

TEST_P(WasmiErrorHandlingTest, Spin)
{
  link_fn("test", "external_call", &good_call);
  auto runtime = get_context()->make_new_runtime(deployed_contract_hash_,
                                                 syscall_handler_.get(), true);

  ASSERT_TRUE(runtime.has_value());
  ASSERT_TRUE(runtime.value() != nullptr);

  auto res = runtime.value()->invoke("spin", 10000);

  EXPECT_FALSE(res.result_);
  EXPECT_EQ(res.result_.error(), InvokeError{InvokeErrorCode::kInvokeOutOfGas});
}

INSTANTIATE_TEST_SUITE_P(WasmEngines, WasmiErrorHandlingTest,
                         ::testing::Values(WasmEngine::kWasmi,
                                           WasmEngine::kWasmtime),
                         [](const ::testing::TestParamInfo<WasmEngine> &info) {
                           return WasmTestFramework::engine_to_string(
                               info.param);
                         });

} // namespace hogd
