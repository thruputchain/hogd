#include <gtest/gtest.h>

#include <stdexcept>

#include "contract.hh"
#include "hogd/base/types.h"
#include "hogd/crypto/crypto.h"
#include "hogd/hogexec/runtime/runtime.h"
#include "hogd/hogexec/runtime/trampoline_result.h"
#include "hogd/hogexec/runtime/wasm/test_load_wasm.h"
#include "hogd/hogexec/runtime/wasm/wasm_test_framework.h"

namespace hogd {

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

TrampolineResult
dummy_syscall_8args(HostCallContext * /*unused*/, uint64_t /*unused*/,
                    uint64_t /*unused*/, uint64_t /*unused*/,
                    uint64_t /*unused*/, uint64_t /*unused*/,
                    uint64_t /*unused*/, uint64_t /*unused*/,
                    uint64_t /*unused*/) noexcept
{
  return {.result = 0, .panic = 0};
}

} // namespace

// Regression test for issue #1464: a contract that imports marmot::syscall
// under the correct module and function names but with the wrong Wasm function
// type should be treated as a deterministic invalid contract (null runtime),
// not escalated into kSystemUnrecoverable.
//
// Both engines are exercised: wasmi already handles type mismatches correctly
// in linker.instantiate() (returns Ok(None) for all instantiation errors —
// see wasmi_runtime.rs:107-112).  The fix was only needed for wasmtime, where
// instantiate_pre() errors other than UnknownImportError were escalated to
// kSystemUnrecoverable.
TEST_P(InstantiateTest, WrongImportTypeSignatureMismatch)
{
  ASSERT_TRUE(link_fn("marmot", "syscall", &dummy_syscall_8args));

  Contract contract(ContractType::kWasmi);
  contract.wasmi_contract() =
      test::load_wasm_from_file("test_wrong_import_type.wasm");
  EXPECT_TRUE(get_context()->verify_contract(contract));

  auto deployed_contract_hash = DeployedContractHash{
      .hash = sha256(contract),
      .contract_size =
          static_cast<uint32_t>(contract.wasmi_contract().wasm.size()),
      .contract_type = ContractType::kWasmi,
  };

  make_syscall_handler(contract);
  auto runtime = get_context()->make_new_runtime(deployed_contract_hash,
                                                 get_syscall_handler());
  ASSERT_TRUE(runtime.has_value())
      << "type-mismatched import caused a SystemError";
  EXPECT_FALSE(runtime.value());
}

// Wrong-import-kind tests (global, table, memory instead of func) are not
// needed: wasm-meter 0.2.0 rejects all non-function imports at instrumentation
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
