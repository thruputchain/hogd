#include <gtest/gtest.h>

#include <cstdint>
#include <memory>

#include "contract.hh"
#include "ffi_enums.hh"
#include "hogd/base/types.h"
#include "hogd/crypto/crypto.h"
#include "hogd/hogexec/common/status.h"
#include "hogd/hogexec/runtime/runtime.h"
#include "hogd/hogexec/runtime/syscall_entry.h"
#include "hogd/hogexec/runtime/trampoline_result.h"
#include "hogd/hogexec/runtime/wasm/test_load_wasm.h"
#include "hogd/hogexec/runtime/wasm/wasm_test_framework.h"
#include "hogd/rust/wasmtime/bindings.h"
#include "syscall_error.hh"
#include "types.hh"

namespace hogd {

namespace {

extern "C" TrampolineResult
throw_unrecoverable_host_error(HostCallContext * /*unused*/) noexcept
{
  return to_trampoline_result(HostFnError{HostFnErrorCode::Unrecoverable});
}

extern "C" TrampolineResult
throw_validation_error(HostCallContext * /*unused*/) noexcept
{
  return to_trampoline_result(HostFnError{HostFnErrorCode::ValidationFailure});
}

extern "C" TrampolineResult
throw_abort_transaction(HostCallContext * /*unused*/) noexcept
{
  return to_trampoline_result(HostFnError{HostFnErrorCode::Abandon});
}

extern "C" TrampolineResult
good_call(HostCallContext * /*unused*/) noexcept
{
  return to_trampoline_result({SyscallErrorCode::kSyscallErrorCodeNone});
}

extern "C" TrampolineResult
exit_call(HostCallContext * /*unused*/) noexcept
{
  return to_trampoline_result(HostFnError{HostFnErrorCode::Exit});
}

extern "C" TrampolineResult
return_syscall(HostCallContext * /*unused*/) noexcept
{
  return to_trampoline_result(HostFnError{HostFnErrorCode::Return});
}

// Wasmi doesn't necessarily insert a "consume gas" instruction into its
// internal bytecode at the end of a call instruction. So if a host function
// uses all gas, it has to return kOutOfGas.
extern "C" TrampolineResult
consume_too_much_gas(HostCallContext *context) noexcept
{
  bool _ = context->runtime->consume_gas(context->runtime->get_available_gas());
  return to_trampoline_result(HostFnError{HostFnErrorCode::OutOfGas});
}

namespace {
Runtime *s_runtime = nullptr;
bool reentrance_hit = false;
} // namespace

extern "C" TrampolineResult
reentrance(HostCallContext * /*unused*/) noexcept
{
  reentrance_hit = true;
  if (s_runtime == nullptr) {
    return to_trampoline_result(HostFnError{HostFnErrorCode::Unrecoverable});
  }

  auto res = s_runtime->invoke("unreachable", UINT64_MAX);
  if (!res.result_.has_value()) {
    return to_trampoline_result(HostFnError{HostFnErrorCode::Unrecoverable});
  }
  return to_trampoline_result({SyscallErrorCode::kSyscallErrorCodeNone});
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

    setup_wasm_context(GetParam());
    link_fn("test", "good_call", &good_call);
  }

  Contract contract_;
  DeployedContractHash deployed_contract_hash_;
};

TEST_P(WasmiErrorHandlingTest, UnlinkedFn)
{
  make_syscall_handler(contract_);
  auto runtime = get_context()->make_new_runtime(deployed_contract_hash_,
                                                 get_syscall_handler());
  ASSERT_TRUE(runtime.has_value());
  EXPECT_TRUE(runtime.value() == nullptr);
}

TEST_P(WasmiErrorHandlingTest, ExitHandling)
{
  link_fn("test", "external_call", &exit_call);
  make_syscall_handler(contract_);
  auto runtime = get_context()->make_new_runtime(deployed_contract_hash_,
                                                 get_syscall_handler());

  ASSERT_TRUE(runtime.has_value());
  ASSERT_TRUE(runtime.value() != nullptr);

  auto res = runtime.value()->invoke("call1", UINT64_MAX);

  EXPECT_FALSE(res.is_root_success());
}

TEST_P(WasmiErrorHandlingTest, HostError)
{
  link_fn("test", "external_call", &throw_unrecoverable_host_error);
  make_syscall_handler(contract_);
  auto runtime = get_context()->make_new_runtime(deployed_contract_hash_,
                                                 get_syscall_handler());

  ASSERT_TRUE(runtime.has_value());
  ASSERT_TRUE(runtime.value() != nullptr);

  auto res = runtime.value()->invoke("call1", UINT64_MAX);
  EXPECT_EQ(res.result_.error(), InvokeError{InvokeErrorCode::Unrecoverable});
}

TEST_P(WasmiErrorHandlingTest, Abandon)
{
  link_fn("test", "external_call", &throw_abort_transaction);
  make_syscall_handler(contract_);
  auto runtime = get_context()->make_new_runtime(deployed_contract_hash_,
                                                 get_syscall_handler());

  ASSERT_TRUE(runtime.has_value());
  ASSERT_TRUE(runtime.value() != nullptr);

  auto res = runtime.value()->invoke("call1", UINT64_MAX);
  EXPECT_EQ(res.result_.error(), InvokeError{InvokeErrorCode::Abandon});
}

TEST_P(WasmiErrorHandlingTest, ValidationError)
{
  link_fn("test", "external_call", &throw_validation_error);
  make_syscall_handler(contract_);
  auto runtime = get_context()->make_new_runtime(deployed_contract_hash_,
                                                 get_syscall_handler());

  ASSERT_TRUE(runtime.has_value());
  ASSERT_TRUE(runtime.value() != nullptr);

  auto res = runtime.value()->invoke("call1", UINT64_MAX);

  EXPECT_EQ(res.result_.error(),
            InvokeError{InvokeErrorCode::ValidationFailure});
}

TEST_P(WasmiErrorHandlingTest, WasmError)
{
  // must link all the fns, required for linking
  link_fn("test", "external_call", &good_call);
  make_syscall_handler(contract_);
  auto runtime = get_context()->make_new_runtime(deployed_contract_hash_,
                                                 get_syscall_handler());

  ASSERT_TRUE(runtime.has_value());
  ASSERT_TRUE(runtime.value() != nullptr);

  auto res = runtime.value()->invoke("call1", UINT64_MAX);

  EXPECT_TRUE(res.result_);

  res = runtime.value()->invoke("unreachable", UINT64_MAX);

  EXPECT_FALSE(res.result_);
  EXPECT_EQ(res.result_.error(), InvokeError{InvokeErrorCode::Deterministic});
}

TEST_P(WasmiErrorHandlingTest, NonexistFn)
{
  // must link all the fns, required for linking
  link_fn("test", "external_call", &good_call);
  make_syscall_handler(contract_);
  auto runtime = get_context()->make_new_runtime(deployed_contract_hash_,
                                                 get_syscall_handler());

  ASSERT_TRUE(runtime.has_value());
  ASSERT_TRUE(runtime.value() != nullptr);

  auto res = runtime.value()->invoke("call1", UINT64_MAX);

  EXPECT_TRUE(res.result_);

  res = runtime.value()->invoke("blah", UINT64_MAX);

  EXPECT_FALSE(res.result_);
  EXPECT_EQ(res.result_.error(), InvokeError{InvokeErrorCode::Deterministic});
}

TEST_P(WasmiErrorHandlingTest, FollowBadWithGood)
{
  link_fn("test", "external_call", &throw_unrecoverable_host_error);
  make_syscall_handler(contract_);
  auto runtime = get_context()->make_new_runtime(deployed_contract_hash_,
                                                 get_syscall_handler());

  ASSERT_TRUE(runtime.has_value());
  ASSERT_TRUE(runtime.value() != nullptr);

  auto res = runtime.value()->invoke("call1", UINT64_MAX);

  EXPECT_FALSE(res.result_);
  EXPECT_EQ(res.result_.error(), InvokeError{InvokeErrorCode::Unrecoverable});

  res = runtime.value()->invoke("call2", UINT64_MAX);

  EXPECT_TRUE(res.result_);
}

TEST_P(WasmiErrorHandlingTest, CheckReturn)
{
  link_fn("test", "external_call", &return_syscall);
  make_syscall_handler(contract_);
  auto runtime = get_context()->make_new_runtime(deployed_contract_hash_,
                                                 get_syscall_handler());

  ASSERT_TRUE(runtime.has_value());
  ASSERT_TRUE(runtime.value() != nullptr);

  auto res = runtime.value()->invoke("call_then_unreachable", UINT64_MAX);

  EXPECT_TRUE(res.result_);
}

TEST_P(WasmiErrorHandlingTest, FollowReallyBadWithGood)
{
  link_fn("test", "external_call", &throw_unrecoverable_host_error);
  make_syscall_handler(contract_);
  auto runtime = get_context()->make_new_runtime(deployed_contract_hash_,
                                                 get_syscall_handler());

  ASSERT_TRUE(runtime.has_value());
  ASSERT_TRUE(runtime.value() != nullptr);

  auto res = runtime.value()->invoke("call1", UINT64_MAX);

  EXPECT_FALSE(res.result_);
  EXPECT_EQ(res.result_.error(), InvokeError{InvokeErrorCode::Unrecoverable});

  res = runtime.value()->invoke("call2", UINT64_MAX);

  EXPECT_TRUE(res.result_);
}

TEST_P(WasmiErrorHandlingTest, Reentrance)
{
  link_fn("test", "external_call", &reentrance);
  make_syscall_handler(contract_);
  auto runtime = get_context()->make_new_runtime(deployed_contract_hash_,
                                                 get_syscall_handler());

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
  make_syscall_handler(null_contract);
  auto runtime = get_context()->make_new_runtime(null_deployed_contract_hash,
                                                 get_syscall_handler());
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

  make_syscall_handler(contract_);
  auto runtime = get_context()->make_new_runtime(deployed_contract_hash_,
                                                 get_syscall_handler());
  ASSERT_TRUE(runtime.has_value());

  EXPECT_FALSE(runtime.value());
}

TEST_P(WasmiErrorHandlingTest, OutOfGas)
{
  link_fn("test", "external_call", &consume_too_much_gas);
  make_syscall_handler(contract_);
  auto runtime = get_context()->make_new_runtime(deployed_contract_hash_,
                                                 get_syscall_handler());

  ASSERT_TRUE(runtime.has_value());
  ASSERT_TRUE(runtime.value() != nullptr);

  auto res = runtime.value()->invoke("call1", UINT64_MAX);

  EXPECT_FALSE(res.result_);
  EXPECT_EQ(res.result_.error(), InvokeError{InvokeErrorCode::OutOfGas});

  res = runtime.value()->invoke("call1", 1);

  EXPECT_FALSE(res.result_);
  EXPECT_EQ(res.result_.error(), InvokeError{InvokeErrorCode::OutOfGas});
}

TEST_P(WasmiErrorHandlingTest, NoGas)
{
  link_fn("test", "external_call", &good_call);
  make_syscall_handler(contract_);
  auto runtime = get_context()->make_new_runtime(deployed_contract_hash_,
                                                 get_syscall_handler());

  ASSERT_TRUE(runtime.has_value());
  ASSERT_TRUE(runtime.value() != nullptr);

  auto res = runtime.value()->invoke("call1", 1);

  EXPECT_FALSE(res.result_);
  EXPECT_EQ(res.result_.error(), InvokeError{InvokeErrorCode::OutOfGas});
}

TEST_P(WasmiErrorHandlingTest, Spin)
{
  link_fn("test", "external_call", &good_call);
  make_syscall_handler(contract_);
  auto runtime = get_context()->make_new_runtime(deployed_contract_hash_,
                                                 get_syscall_handler());

  ASSERT_TRUE(runtime.has_value());
  ASSERT_TRUE(runtime.value() != nullptr);

  auto res = runtime.value()->invoke("spin", 10000);

  EXPECT_FALSE(res.result_);
  EXPECT_EQ(res.result_.error(), InvokeError{InvokeErrorCode::OutOfGas});
}

TEST_P(WasmiErrorHandlingTest, ResolveContractReturnsNull)
{
  link_fn("test", "external_call", &good_call);
  make_syscall_handler(contract_);

  // Hash not in newly_created_contracts_ or the DB — resolve_contract returns
  // Ok(nullptr) because BlockSnapshotServer returns kRPCNotFound.
  DeployedContractHash nonexistent_hash{
      .hash = SHA256Hash{},
      .contract_size = 100,
      .contract_type = ContractType::kWasmi,
  };

  auto runtime =
      get_context()->make_new_runtime(nonexistent_hash, get_syscall_handler());
  ASSERT_FALSE(runtime.has_value());
  EXPECT_EQ(runtime.error(), SystemError{SystemErrorCode::Unrecoverable});
}

INSTANTIATE_TEST_SUITE_P(WasmEngines, WasmiErrorHandlingTest,
                         ::testing::Values(WasmEngine::kWasmi,
                                           WasmEngine::kWasmtime),
                         [](const ::testing::TestParamInfo<WasmEngine> &info) {
                           return WasmTestFramework::engine_to_string(
                               info.param);
                         });

namespace {

extern "C" ContractGetResult
return_null_empty_contract(void * /*unused*/,
                           const uint8_t * /*unused*/) noexcept
{
  return ContractGetResult{.err_code = 0, .data = nullptr, .data_len = 0};
}

extern "C" ContractGetResult
return_non_null_empty_contract(void * /*unused*/,
                               const uint8_t * /*unused*/) noexcept
{
  static constexpr uint8_t kSentinel = 0;
  return ContractGetResult{.err_code = 0, .data = &kSentinel, .data_len = 0};
}

} // namespace

TEST(WasmtimeRuntimeFFITest, EmptyContractNullDataAndEmptySliceDiverge)
{
  auto global_context =
      std::unique_ptr<void, decltype(&free_wasmtime_global_context)>(
          new_wasmtime_global_context(1), &free_wasmtime_global_context);
  ASSERT_NE(global_context.get(), nullptr);

  SHA256Hash hash{};
  uint8_t user_context = 0;

  // Null data pointer: treated as "contract not found" -> null runtime
  NewWasmtimeRuntimeResult null_result;
  {
    auto ctx = std::unique_ptr<void, decltype(&free_wasmtime_context)>(
        new_wasmtime_context(&return_null_empty_contract, global_context.get()),
        &free_wasmtime_context);
    ASSERT_NE(ctx.get(), nullptr);

    null_result = new_wasmtime_runtime(hash.data(), &user_context, ctx.get());
  }

  // Non-null data pointer with data_len == 0: currently compiled as
  // empty Wasm module, fails Module::new -> kSystemUnrecoverable
  NewWasmtimeRuntimeResult non_null_result;
  {
    auto ctx = std::unique_ptr<void, decltype(&free_wasmtime_context)>(
        new_wasmtime_context(&return_non_null_empty_contract,
                             global_context.get()),
        &free_wasmtime_context);
    ASSERT_NE(ctx.get(), nullptr);

    non_null_result =
        new_wasmtime_runtime(hash.data(), &user_context, ctx.get());
  }

  EXPECT_EQ(null_result.err_code, 0);
  EXPECT_EQ(null_result.runtime_pointer, nullptr);

  // Both paths must classify identically: empty contract -> null runtime
  EXPECT_EQ(non_null_result.err_code, 0);
  EXPECT_EQ(non_null_result.runtime_pointer, nullptr);
}

} // namespace hogd
