#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <gtest/gtest.h>
#include <memory>
#include <span>
#include <unordered_map>
#include <utility>

#include "exec_result.hh"
#include "ffi_enums.hh"
#include "hogd/base/types.h"
#include "hogd/hog_exec/common/status.h"
#include "hogd/hog_exec/runtime/gas_cost_table.h"
#include "hogd/hog_exec/runtime/native_contract_test_framework.h"
#include "hogd/hog_exec/runtime/native_runtime.h"
#include "hogd/hog_exec/runtime/native_syscall_context.h"
#include "hogd/hog_exec/runtime/runtime.h"
#include "invoke_args.hh"
#include "rpc_types.hh"
#include "syscall.hh"
#include "syscall_error.hh"
#include "transaction.hh"
#include "types.hh"

namespace hogd {
// NOLINTBEGIN(bugprone-exception-escape)

// Regression test for issue #3802: repeated same-frame GetReturnedBuffer copies
// must not be host-gas-free.
//
// Before #3750, syscall_GetReturnedBuffer_inner returned WrappedGasAmount{0}
// and the entry path added nothing, so a parent that invoked a child returning
// kMaxReturnBufferLen bytes could copy those retained bytes back out again and
// again for only the guest instruction gas of the loop. Since #3750 the syscall
// entry path charges kSyscallBaseCost on every crossing regardless of what the
// body reports, so each retrieval is now priced.
//
// The native harness charges no per-instruction guest gas, so the only cost of
// an extra GetReturnedBuffer call is that fixed crossing charge. Running the
// same parent with one retrieval and with many therefore isolates it exactly:
// the gas delta is (extra retrievals) * kSyscallBaseCost, and nothing else.
// NOLINTNEXTLINE(misc-use-internal-linkage)
class GetReturnedBufferGas3802Test : public ::testing::Test,
                                     public NativeContractTestFramework {
public:
  GetReturnedBufferGas3802Test()
    : NativeContractTestFramework(
          false, BlockExecutionMode::kBlockExecutionModeProposal,
          TestDB::kHogDb)
  {}

protected:
  // kMaxReturnBufferLen; a maximum-sized child return buffer is the worst case
  // the report describes.
  constexpr static uint32_t kBufferLen = 1024;

  // Deploys a parent that invokes a child returning kBufferLen bytes, then
  // reads the retained buffer back `retrievals` times, and returns the total
  // gas the transaction consumed. `seed` keeps each run on its own addresses so
  // repeated runs share no deployment or replay-cache state.
  GasAmount run(uint32_t retrievals, uint8_t seed)
  {
    const ContractAddress caller = make_contract_address(seed);
    const ContractAddress child =
        make_contract_address(static_cast<uint8_t>(seed + 1));

    std::array<std::byte, kBufferLen> expected{};
    for (size_t i = 0; i < expected.size(); ++i) {
      expected[i] = static_cast<std::byte>(i & 0xFF);
    }

    std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns_child;
    fns_child.emplace(
        0, [&](NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
          auto ret = ctx.syscall<SyscallNo::kReturn>(
              std::span<const std::byte>{expected}, false,
              std::span<const std::byte>{});
          EXPECT_FALSE(ret.has_value()); // Return yields a non-recoverable status
          return {};
        });

    std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns_caller;
    fns_caller.emplace(
        0, [&](NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
          const CopyableInvokeInputs inputs{
              .entrypoint_id = 0,
              .gas_limit = UINT64_MAX,
              .flags = 0,
          };
          CopyableInvokeOutputs outputs{};
          auto invoke_res = ctx.syscall<SyscallNo::kInvoke>(
              child, std::span<const std::byte>{},
              std::span<uint32_t const>{}, inputs, outputs);
          EXPECT_TRUE(invoke_res.has_value());
          EXPECT_EQ(invoke_res.value(),
                    SyscallErrorCode::kSyscallErrorCodeNone);
          EXPECT_EQ(outputs.return_buffer_len, kBufferLen);

          for (uint32_t i = 0; i < retrievals; ++i) {
            std::array<std::byte, kBufferLen> out{};
            RuntimeMemoryLength written = 0;
            RuntimeMemoryLength ids_written = 0;
            auto get_res = ctx.syscall<SyscallNo::kGetReturnedBuffer>(
                std::span<std::byte>{out}, written, std::span<std::byte>{},
                ids_written);
            EXPECT_TRUE(get_res.has_value());
            EXPECT_EQ(get_res.value(),
                      SyscallErrorCode::kSyscallErrorCodeNone);
            // Every retrieval copies the full retained buffer: this is the
            // length-proportional work the report showed going unpriced.
            EXPECT_EQ(written, kBufferLen);
            EXPECT_TRUE(std::ranges::equal(out, expected));
          }
          return {};
        });

    std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
    contract_map.emplace(0, std::make_unique<NativeContract>(
                                std::move(fns_caller)));
    contract_map.emplace(1, std::make_unique<NativeContract>(
                                std::move(fns_child)));
    auto runtime_context =
        std::make_unique<NativeContext>(std::move(contract_map));

    auto caller_xdr = make_native_contract(0);
    write_contract(caller_xdr);
    write_deployment(caller, caller_xdr);
    auto child_xdr = make_native_contract(1);
    write_contract(child_xdr);
    write_deployment(child, child_xdr);

    setup_grant_storage_token(caller, replay_cache_cost_charged(1));

    auto handler = make_new_system_call_handler(
        Transaction{.invocation =
                        Invocation{.invoked_address = caller,
                                   .entrypoint = entrypoint_from_method_id(0),
                                   .gas_limit = UINT64_MAX,
                                   .calldata = {}},
                    .uniquifier = make_new_uniquifier()},
        *runtime_context);
    const auto result = handler.root_invoke();
    EXPECT_TRUE(result.is_root_success());
    EXPECT_EQ(handler.try_commit_transaction(), ExecResult::ExecResultOK);
    return result.gas_consumed_;
  }
};

TEST_F(GetReturnedBufferGas3802Test, RepeatedRetrievalsChargeHostGasPerCrossing)
{
  constexpr uint32_t kSingle = 1;
  constexpr uint32_t kRepeated = 64;

  const GasAmount single_gas = run(kSingle, 0);
  const GasAmount repeated_gas = run(kRepeated, 10);

  // The extra retrievals are not free (the bug was that they were), and each
  // one costs exactly the fixed syscall crossing and nothing else.
  ASSERT_GT(repeated_gas, single_gas);
  EXPECT_EQ(repeated_gas - single_gas,
            (kRepeated - kSingle) * kSyscallBaseCost);
}

// NOLINTEND(bugprone-exception-escape)
} // namespace hogd
