#include <gtest/gtest.h>

#include <cstdint>
#include <stdexcept>

#include "contract.hh"
#include "ffi_enums.hh"
#include "hogd/base/types.h"
#include "hogd/crypto/crypto.h"
#include "hogd/hogexec/common/status.h"
#include "hogd/hogexec/runtime/runtime.h"
#include "hogd/hogexec/runtime/runtime_exports.h"
#include "hogd/hogexec/runtime/wasm/test_load_wasm.h"
#include "hogd/hogexec/runtime/wasm/wasm_test_framework.h"

namespace hogd {

// NOLINTNEXTLINE(misc-use-internal-linkage)
class InstantiateTest : public ::testing::TestWithParam<WasmEngine>,
                        public WasmTestFramework {
protected:
  void SetUp() override { setup_wasm_context(GetParam()); }
};

TEST_P(InstantiateTest, MissingImportFail)
{
  Contract contract(ContractType::kWasmi);
  contract.wasmi_contract() = test::load_wasm_from_file("test_invoke.wasm");
  auto deployed_contract_hash = DeployedContractHash{
      .hash = sha256(contract),
      .contract_size =
          static_cast<uint32_t>(contract.wasmi_contract().wasm.size()),
      .contract_type = ContractType::kWasmi,
  };

  make_syscall_handler(contract);
  auto runtime = get_context()->make_new_runtime(deployed_contract_hash,
                                                 get_syscall_handler());
  ASSERT_TRUE(runtime.has_value());
  EXPECT_FALSE(runtime.value());
}

TEST_P(InstantiateTest, InstantiateFail)
{
  Contract contract(ContractType::kWasmi);
  contract.wasmi_contract() =
      test::load_wasm_from_file("test_instantiate_fail.wasm");
  make_syscall_handler(contract);
  auto deployed_contract_hash = DeployedContractHash{
      .hash = sha256(contract),
      .contract_size =
          static_cast<uint32_t>(contract.wasmi_contract().wasm.size()),
      .contract_type = ContractType::kWasmi,
  };
  auto runtime = get_context()->make_new_runtime(deployed_contract_hash,
                                                 get_syscall_handler());
  ASSERT_TRUE(runtime.has_value());
  EXPECT_FALSE(runtime.value());
}

TEST_P(InstantiateTest, InstantiateUnexportedMemory)
{
  Contract contract(ContractType::kWasmi);
  contract.wasmi_contract() =
      test::load_wasm_from_file("test_instantiate_unexported_memory.wasm");
  make_syscall_handler(contract);
  auto deployed_contract_hash = DeployedContractHash{
      .hash = sha256(contract),
      .contract_size =
          static_cast<uint32_t>(contract.wasmi_contract().wasm.size()),
      .contract_type = ContractType::kWasmi,
  };
  auto runtime = get_context()->make_new_runtime(deployed_contract_hash,
                                                 get_syscall_handler());
  ASSERT_TRUE(runtime.has_value());

  EXPECT_FALSE(runtime.value());
}

// Modules with a start section are now rejected at instrumentation time
// by wasm_meter (which cannot set the stack budget before start runs).
TEST_P(InstantiateTest, InstantiateNoStart)
{
  EXPECT_THROW(test::load_wasm_from_file("test_no_start.wasm"),
               std::runtime_error);
}

// Regression test for audit issue #1: instantiation of a module with an
// out-of-bounds data segment should return a null runtime, not crash.
// The data segment is at offset 70000, beyond 1-page (65536 byte) memory.
TEST_P(InstantiateTest, InstantiateOobDataSegment)
{
  Contract contract(ContractType::kWasmi);
  contract.wasmi_contract() =
      test::load_wasm_from_file("test_oob_data_segment.wasm");
  make_syscall_handler(contract);
  auto deployed_contract_hash = DeployedContractHash{
      .hash = sha256(contract),
      .contract_size =
          static_cast<uint32_t>(contract.wasmi_contract().wasm.size()),
      .contract_type = ContractType::kWasmi,
  };
  auto runtime = get_context()->make_new_runtime(deployed_contract_hash,
                                                 get_syscall_handler());
  ASSERT_TRUE(runtime.has_value());
  EXPECT_FALSE(runtime.value());
}

// Regression test for audit issue #1 variant: active element segment
// initialization with an out-of-bounds table offset should return a null
// runtime, not crash.
TEST_P(InstantiateTest, InstantiateOobElementSegment)
{
  Contract contract(ContractType::kWasmi);
  contract.wasmi_contract() =
      test::load_wasm_from_file("test_oob_element_segment.wasm");
  make_syscall_handler(contract);
  auto deployed_contract_hash = DeployedContractHash{
      .hash = sha256(contract),
      .contract_size =
          static_cast<uint32_t>(contract.wasmi_contract().wasm.size()),
      .contract_type = ContractType::kWasmi,
  };
  auto runtime = get_context()->make_new_runtime(deployed_contract_hash,
                                                 get_syscall_handler());
  ASSERT_TRUE(runtime.has_value());
  EXPECT_FALSE(runtime.value());
}

namespace {

extern "C" TrampolineResult
dummy_syscall_8args(ExternHostCallContext /*unused*/, WasmContext /*unused*/,
                    uint64_t /*unused*/, uint64_t /*unused*/,
                    uint64_t /*unused*/, uint64_t /*unused*/,
                    uint64_t /*unused*/, uint64_t /*unused*/,
                    uint64_t /*unused*/, uint64_t /*unused*/)
{
  return {.result_code = 0, .panic = 0, .gas_consumed = 0};
}

} // namespace

// Regression test for issue #1464 / #2021: a contract that imports
// marmot::syscall with the wrong arity is now rejected at verify_contract
// time, before it can be deployed.
TEST_P(InstantiateTest, WrongImportTypeRejectedAtVerification)
{
  ASSERT_TRUE(link_fn("marmot", "syscall", &dummy_syscall_8args));

  Contract contract(ContractType::kWasmi);
  contract.wasmi_contract() =
      test::load_wasm_from_file("test_wrong_import_type.wasm");
  EXPECT_NE(get_context()->verify_contract(contract), InvokeErrorCode::None);
}

// Regression guard for #1533 / #2021: a contract that imports a name the
// host does not register must be rejected at verify_contract time, before
// deployment metadata can be committed.
TEST_P(InstantiateTest, UnknownImportRejectedAtVerification)
{
  ASSERT_TRUE(link_fn("marmot", "syscall", &dummy_syscall_8args));

  Contract contract(ContractType::kWasmi);
  contract.wasmi_contract() =
      test::load_wasm_from_file("test_unknown_import.wasm");

  EXPECT_NE(get_context()->verify_contract(contract), InvokeErrorCode::None);
}

// Regression test for issue #1472: both engines currently allow a method-name
// export with the wrong ABI through verification and instantiation, so the
// invoke path must classify the signature mismatch as deterministic instead of
// unrecoverable.
TEST_P(InstantiateTest, ParamEntryPointSignatureMismatchIsDeterministic)
{
  Contract contract(ContractType::kWasmi);
  contract.wasmi_contract() =
      test::load_wasm_from_file("test_param_entrypoint.wasm");
  EXPECT_EQ(get_context()->verify_contract(contract), InvokeErrorCode::None);

  auto deployed_contract_hash = DeployedContractHash{
      .hash = sha256(contract),
      .contract_size =
          static_cast<uint32_t>(contract.wasmi_contract().wasm.size()),
      .contract_type = ContractType::kWasmi,
  };

  make_syscall_handler(contract);
  auto runtime = get_context()->make_new_runtime(deployed_contract_hash,
                                                 get_syscall_handler());
  ASSERT_TRUE(runtime.has_value()) << "bad entrypoint ABI caused a SystemError";
  ASSERT_TRUE(runtime.value());

  auto result = runtime.value()->invoke(Runtime::invoke_name(1), UINT64_MAX);

  ASSERT_FALSE(result.result_);
  EXPECT_EQ(result.result_.error(),
            InvokeError{InvokeErrorCode::Deterministic});
}

// Wrong-import-kind tests (global, table, memory instead of func) are not
// needed: wasm-meter 0.3.0 rejects all non-function imports at instrumentation
// and verification time ("only function imports are allowed"), so the
// entity-kind mismatch error paths in wasmtime's typecheck are unreachable
// for deployed contracts.

INSTANTIATE_TEST_SUITE_P(WasmEngines, InstantiateTest,
                         ::testing::Values(WasmEngine::kWasmi,
                                           WasmEngine::kWasmtime),
                         [](const ::testing::TestParamInfo<WasmEngine> &info) {
                           return WasmTestFramework::engine_to_string(
                               info.param);
                         });

} // namespace hogd
