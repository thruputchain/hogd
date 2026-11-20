// get_rv32_stack_usage(): the Debug-only rv32 stack diagnostic
// (HOGD_RV32_STACK_USAGE, see hogd/hog_exec/runtime/rv32/stack_usage.h)
// reaches the FFI as one record per rv32 invocation, carrying the first and
// the lowest sp the guest wrote. Built without the diagnostic, the entry point
// exists and reports no records.

#include <cstdint>
#include <cstring>
#include <vector>

#include <gtest/gtest.h>

#include "contract.hh"
#include "hogd/base/types.h"
#include "hogd/crypto/crypto.h"
#include "hogd/hog_exec/runtime/contract_trait.h"
#include "hogd/hog_exec/runtime/rv32/stack_usage.h"
#include "hogd/hog_exec/runtime/rv32/test_artifact_builder.h"
#include "hogd/hog_exec/runtime/rv32/test_program_builder.h"
#include "hogd/test_ffi/test_bindings.h"
#include "hogd/test_ffi/test_ffi_test_helpers.h"
#include "transaction.hh"
#include "types.hh"
#include "x23/cleanup.h"
#include "x23/serialize.h"

namespace hogd {

namespace {

constexpr uint32_t kEcallWord = 0x00000073U;
constexpr int32_t kReturnSyscallNo = 6;
constexpr uint64_t kMethodId = 0x1234;

// a0 = Return, a1..a5 = 0, ecall: the guest returns an empty buffer.
void
append_return(std::vector<uint32_t> &words)
{
  uint8_t reg = 10;
  for (const int32_t imm : {kReturnSyscallNo, 0, 0, 0, 0, 0}) {
    words.push_back(rv32::test_program::addi(reg++, 0, imm));
  }
  words.push_back(kEcallWord);
}

// Deploys `words` as an rv32 contract at the zero address, runs it as the root
// invocation of a transaction and returns the stack records the handler holds
// afterwards. Expects the guest to Return.
GetRv32StackUsageResult
run_and_read_records(std::vector<uint32_t> const &words)
{
  auto *db = make_db();
  EXPECT_NE(db, nullptr);
  x23::Defer d([db] { free_db(db); });

  auto *rc = make_runtime_context();
  EXPECT_NE(rc, nullptr);
  x23::Defer d2([rc] { close_runtime_context(rc); });

  Contract contract(ContractType::kRv32);
  contract.rv32_contract().data.append_range(
      rv32::test_artifact::build_artifact(words,
                                          std::vector<uint8_t>(8 * 1024, 0)));
  auto bytes = x23::serialize(contract);
  EXPECT_EQ(setup_create_contract(rc, db, bytes.data(), bytes.size()),
            kTestFFIErrorCodeNone);

  ContractAddress deployment_addr{};
  EXPECT_EQ(setup_deploy_contract(db, deployment_addr.data(),
                                  sha256(contract).data(), bytes.size()),
            kTestFFIErrorCodeNone);
  EXPECT_EQ(setup_grant_contract_storage_tokens(db, deployment_addr.data(),
                                                1'000'000),
            kTestFFIErrorCodeNone);

  SignedTransaction stx{
      .tx = {.invocation =
                 {
                     .invoked_address = deployment_addr,
                     .entrypoint = entrypoint_from_method_id(kMethodId),
                     .gas_limit = 1'000'000,
                     .calldata = {},
                 },
             .uniquifier = {.block_expiry_height = INT64_MAX, .nonce = 0}},
      .signatures = {},
  };
  auto *syscall_handler = make_syscall_handler(db, rc, stx);
  x23::Defer d3([syscall_handler] { close_syscall_handler(syscall_handler); });

  auto invoke_result = root_invoke(syscall_handler, 0);
  x23::Defer d4([&] { free_ffi_data_buffer(invoke_result.serialized_output); });
  x23::Defer d5([&] { free_ffi_data_buffer(invoke_result.serialized_trace); });
  x23::Defer d6([&] { free_ffi_data_buffer(invoke_result.serialized_events); });
  EXPECT_EQ(invoke_result.ffi_error_code, kTestFFIErrorCodeNone);
  EXPECT_NE(invoke_result.serialized_output.data, nullptr);
  InvocationFrameOutput output;
  x23::deserialize(&output, x23::BufView{invoke_result.serialized_output.data,
                                         invoke_result.serialized_output.len});
  EXPECT_EQ(output.status_code(), InvocationFrameStatusCode::Return);

  return get_rv32_stack_usage(syscall_handler);
}

FFIRv32StackUsage
single_record(GetRv32StackUsageResult const &result)
{
  FFIRv32StackUsage record{};
  EXPECT_EQ(result.ffi_error_code, kTestFFIErrorCodeNone);
  EXPECT_EQ(result.records.len, sizeof(record));
  if (result.records.len == sizeof(record)) {
    std::memcpy(&record, result.records.data, sizeof(record));
  }
  return record;
}

} // namespace

TEST(Rv32StackUsageTest, ReportsTheInitialAndLowestSpTheGuestWrote)
{
  // Establish sp at 4096, push 32 then 64 bytes, pop both, then Return. The
  // deepest point, 4000, is restored before the ecall, so only a per-write
  // record can see it. The addi x1 between lui and the first push keeps the
  // pair from lowering to op_lui_addi, so the initial value is the lui's.
  std::vector<uint32_t> words = {
      rv32::test_program::u_type(0x37U, 2, 0x1000U), // lui  sp, 1 -> 4096
      rv32::test_program::addi(1, 0, 7),             // not sp
      rv32::test_program::addi(2, 2, -32),           // -> 4064
      rv32::test_program::addi(2, 2, -64),           // -> 4000 (deepest)
      rv32::test_program::addi(2, 2, 96),            // -> 4096
  };
  append_return(words);

  auto result = run_and_read_records(words);
  x23::Defer free_records([&] { free_ffi_data_buffer(result.records); });
#if HOGD_RV32_STACK_USAGE_ENABLED
  const FFIRv32StackUsage record = single_record(result);
  const ContractAddress deployment_addr{};
  EXPECT_EQ(std::memcmp(record.contract_address, deployment_addr.data(),
                        sizeof(record.contract_address)),
            0);
  EXPECT_EQ(record.method_id, kMethodId);
  EXPECT_EQ(record.sp_initial, 4096U);
  EXPECT_EQ(record.sp_min, 4000U);
  EXPECT_EQ(record.sp_initial - record.sp_min, 96U); // the usage
#else
  EXPECT_EQ(result.ffi_error_code, kTestFFIErrorCodeNone);
  EXPECT_EQ(result.records.data, nullptr);
  EXPECT_EQ(result.records.len, 0U);
#endif
}

TEST(Rv32StackUsageTest, GuestThatNeverWritesSpReportsTheSentinel)
{
  std::vector<uint32_t> words;
  append_return(words);

  auto result = run_and_read_records(words);
  x23::Defer free_records([&] { free_ffi_data_buffer(result.records); });
#if HOGD_RV32_STACK_USAGE_ENABLED
  const FFIRv32StackUsage record = single_record(result);
  EXPECT_EQ(record.method_id, kMethodId);
  EXPECT_EQ(record.sp_initial, rv32::kSpUnwritten);
  EXPECT_EQ(record.sp_min, rv32::kSpUnwritten);
  EXPECT_EQ(record.sp_min, 0xFFFFFFFFU); // the value test_bindings.h documents
#else
  EXPECT_EQ(result.ffi_error_code, kTestFFIErrorCodeNone);
  EXPECT_EQ(result.records.len, 0U);
#endif
}

TEST(Rv32StackUsageTest, NullHandlerIsAnInvalidPointer)
{
  auto result = get_rv32_stack_usage(nullptr);
  EXPECT_EQ(result.ffi_error_code, kTestFFIErrorCodeInvalidPointer);
  EXPECT_EQ(result.records.data, nullptr);
  EXPECT_EQ(result.records.len, 0U);
}

} // namespace hogd
