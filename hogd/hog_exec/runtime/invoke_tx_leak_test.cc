#include <gtest/gtest.h>
#include <span>

#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>

#include "contract.hh"
#include "db_conflict.hh"
#include "exec_result.hh"
#include "ffi_enums.hh"
#include "hogd/base/network_config_params.h"
#include "hogd/base/types.h"
#include "hogd/crypto/crypto.h"
#include "hogd/hog_exec/common/status.h"
#include "hogd/hog_exec/precompiles/native_contract_builder.h"
#include "hogd/hog_exec/runtime/contract_trait.h"
#include "hogd/hog_exec/runtime/item/deployed_contract.h"
#include "hogd/hog_exec/runtime/multiplexed_context.h"
#include "hogd/hog_exec/runtime/native_contract_test_framework.h"
#include "hogd/hog_exec/runtime/native_runtime.h"
#include "hogd/hog_exec/runtime/native_syscall_context.h"
#include "hogd/hog_exec/runtime/prepared_contract.h"
#include "hogd/hog_exec/runtime/runtime.h"
#include "hogd/hog_exec/runtime/rv32/program.h"
#include "hogd/hog_exec/runtime/rv32/rv32_contract.h"
#include "hogd/hog_exec/runtime/rv32/test_artifact_builder.h"
#include "hogd/hog_exec/runtime/rv32/test_program_builder.h"
#include "hogd/hog_exec/runtime/syscall_handler.h"
#include "hogd/hog_exec/runtime/syscall_inner.h"
#include "rpc_types.hh"
#include "syscall.hh"
#include "syscall_error.hh"
#include "transaction.hh"
#include "types.hh"
#include "x23/async.h"

namespace hogd {

// Regression test for #2192: when the final contract batch commit exceeds
// the SRPC message cap, try_commit_transaction must abort the open HogDb
// InvokeTX so that the BlockTX refcount drops to zero and the block can
// be cleanly discarded.
//
// Strategy: shrink the message cap so the final BorrowedContractBatch
// serialization fails, discard the block, then try a read on the old
// block ID. If the InvokeTX was leaked, the BlockTX stays alive and the
// read succeeds. If properly cleaned up, the read returns Abandon.

namespace {

class NoopRv32Context : public RuntimeContext {
public:
  std::expected<void, ContractRejectCode>
  verify_contract(Contract const & /*unused*/,
                  NetworkConfigParams const & /*params*/,
                  PreparedContract &out) const override
  {
    // Hand back a placeholder image, as a real rv32 verification would: an
    // accepted attachment must look prepared. Its contents are irrelevant --
    // these tests never invoke the deployed contracts -- and it cannot be
    // built from the attachment itself, which is opaque placeholder bytes and,
    // in the size-limit test, larger than any valid artifact can be.
    ContractBlob placeholder;
    placeholder.data.append_range(
        rv32::test_artifact::build_artifact({rv32::test_program::addi(0, 0, 0)},
                                            std::span<const uint8_t>{}));
    out.rv32.emplace(
        *Rv32Contract::create(placeholder, rv32::Rv32Limits::defaults()));
    return {};
  }

  x23::Async<SystemStatus<std::unique_ptr<Runtime>>>
  make_new_runtime(DeployedContract const & /*unused*/,
                   SystemCallHandler * /*unused*/) const override
  {
    co_return nullptr;
  }
};

} // namespace

TEST(InvokeTxLeakTest, OversizedFinalCommitAbortsInvokeTx)
{
  // Scoped to this framework's exec-side client rather than set on
  // x23::default_max_message_size, which racing server threads also read.
  NativeContractTestFramework fw(
      /*log=*/false, BlockExecutionMode::kBlockExecutionModeProposal,
      TestDB::kHogDb, /*empty_genesis=*/false,
      /*genesis_cost_model=*/std::nullopt,
      /*genesis_network_config=*/std::nullopt, /*db_max_message_size=*/4096);
  fw.run_genesis_initialization(
      NativeContractTestFramework::make_compact_test_genesis_configuration());

  Contract contract_to_deploy(ContractType::kRv32);
  contract_to_deploy.rv32_contract().data.append_range(std::string(8192, 'x'));
  const auto contract_hash = sha256(contract_to_deploy);

  constexpr static SHA256Hash kDeployNonce = []() {
    SHA256Hash nonce{};
    nonce[0] = 0x92;
    return nonce;
  }();

  constexpr auto kDeployerAddr =
      NativeContractTestFramework::make_contract_address(1);

  auto deployed_addr =
      SystemCallHandler::static_get_deploy_address(kDeployerAddr, kDeployNonce,
                                                   contract_hash);

  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns;
  fns.emplace(0,
              [contract_hash](
                  NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
                ContractAddress deployed{};
                auto r = co_await ctx.syscall<SyscallNo::kDeployContract>(
                    kDeployNonce, contract_hash, deployed);
                if (!r.has_value())
                  co_return NativeContractBuilder::host_fn_error_conversion(
                      r.error());
                if (r.value() != SyscallErrorCode::kSyscallErrorCodeNone)
                  co_return InvokeError{InvokeErrorCode::Deterministic};
                co_return InvokeStatus<void>{};
              });

  auto native = std::make_unique<NativeContract>(std::move(fns));
  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
  contract_map.emplace(0, std::move(native));
  NativeContext native_context(std::move(contract_map));
  NoopRv32Context noop_context;
  MultiplexedContext context(
      std::unordered_map<ContractType, std::pair<RuntimeContext const *, bool>>{
          {ContractType::kNative, {&native_context, false}},
          {ContractType::kRv32, {&noop_context, false}}});

  auto deployer_contract = NativeContractTestFramework::make_native_contract(0);
  fw.write_contract(deployer_contract);
  fw.write_deployment(kDeployerAddr, deployer_contract);
  fw.setup_grant_storage_token(kDeployerAddr, 1'000'000'000);
  fw.setup_grant_storage_token(deployed_addr, 1'000'000'000);

  BlockID old_block = fw.get_block_number();

  {
    auto handler = fw.make_new_system_call_handler(
        Transaction{
            .invocation = Invocation{.invoked_address = kDeployerAddr,
                                     .entrypoint = entrypoint_from_method_id(0),
                                     .gas_limit = UINT64_MAX,
                                     .calldata = {}},
            .uniquifier = fw.make_new_uniquifier(),
            .contracts_to_deploy = {contract_to_deploy},
        },
        context);

    auto res = handler.root_invoke()();
    ASSERT_TRUE(res.is_root_success());

    auto commit_res = handler.try_commit_transaction()();
    EXPECT_EQ(commit_res, ExecResult::ExecResultUnrecoverable);
  }

  // Discard the block. If the InvokeTX was properly aborted, the BlockTX
  // refcount is zero and the block can be cleaned up. If it leaked, the
  // BlockTX stays alive.
  fw.discard_block();

  // Probe the old block ID with a per-block read; get_db_tip no longer
  // consults per-block state, so it cannot probe BlockTX liveness. If the
  // BlockTX was garbage collected (InvokeTX properly closed), this returns
  // Abandon. If the leaked InvokeTX kept the BlockTX alive, this succeeds.
  auto probe = fw.get_db_proxy().get_snapshot_item(
      old_block,
      NativeContractTestFramework::make_contract_data_addrkey(kDeployerAddr,
                                                              "leak_probe"))();
  EXPECT_FALSE(probe.has_value())
      << "BlockTX should be gone after discard — leaked InvokeTX kept it alive";
}

} // namespace hogd
