#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>

#include "contract.hh"
#include "exec_result.hh"
#include "hogd/base/types.h"
#include "hogd/crypto/crypto.h"
#include "hogd/hogexec/common/status.h"
#include "hogd/hogexec/runtime/contract_trait.h"
#include "hogd/hogexec/runtime/multiplexed_context.h"
#include "hogd/hogexec/runtime/native_contract_test_framework.h"
#include "hogd/hogexec/runtime/native_runtime.h"
#include "hogd/hogexec/runtime/native_syscall_context.h"
#include "hogd/hogexec/runtime/runtime.h"
#include "hogd/hogexec/runtime/syscall_handler.h"
#include "hogd/hogexec/runtime/syscall_inner.h"
#include "transaction.hh"
#include "types.hh"
#include "x23/rpc_common.h"

namespace hogd {

// Regression test for #2192: when the final contract batch commit exceeds
// the SRPC message cap, try_commit_transaction must abort the open HogDB
// InvokeTX so that the BlockTX refcount drops to zero and the block can
// be cleanly discarded.
//
// Strategy: shrink the message cap so the final BorrowedContractBatch
// serialization fails, discard the block, then try a read on the old
// block ID. If the InvokeTX was leaked, the BlockTX stays alive and the
// read succeeds. If properly cleaned up, the read returns Abandon.

namespace {

class NoopWasmiContext : public RuntimeContext {
public:
  InvokeErrorCode
  verify_contract(Contract const & /*contract*/) const override
  {
    return InvokeErrorCode::None;
  }

  SystemStatus<std::unique_ptr<Runtime>>
  make_new_runtime(DeployedContractHash const & /*hash*/,
                   SystemCallHandler * /*handler*/) override
  {
    return nullptr;
  }
};

} // namespace

TEST(InvokeTxLeakTest, OversizedFinalCommitAbortsInvokeTx)
{
  const std::size_t saved_max = x23::default_max_message_size;
  x23::default_max_message_size = 4096;

  NativeContractTestFramework fw(
      /*log=*/false, BlockExecutionMode::kBlockExecutionModeProposal,
      TestDB::kHogDB);

  Contract contract_to_deploy(ContractType::kWasmi);
  contract_to_deploy.wasmi_contract().wasm.append_range(
      std::string(8192, 'x'));
  const auto contract_hash = sha256(contract_to_deploy);

  constexpr static SHA256Hash kDeployNonce = []() {
    SHA256Hash nonce{};
    nonce[0] = 0x92;
    return nonce;
  }();

  constexpr auto kDeployerAddr =
      NativeContractTestFramework::make_contract_address(1);

  auto deployed_addr = SystemCallHandler::static_get_deploy_address(
      kDeployerAddr, kDeployNonce, contract_hash);

  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns;
  fns.emplace(
      0,
      [contract_hash](
          NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
        auto r = ctx.handler().deploy_contract(kDeployNonce, contract_hash);
        if (!r.has_value())
          return r.error().to_invoke_error();
        if (!r.value().has_value())
          return InvokeError{InvokeErrorCode::Deterministic};
        return {};
      });

  auto native = std::make_unique<NativeContract>(std::move(fns));
  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
  contract_map.emplace(0, std::move(native));
  NativeContext native_context(std::move(contract_map));
  NoopWasmiContext wasmi_context;
  MultiplexedContext context(
      std::unordered_map<ContractType, std::pair<RuntimeContext *, bool>>{
          {ContractType::kNative, {&native_context, false}},
          {ContractType::kWasmi, {&wasmi_context, false}}});

  auto deployer_contract =
      NativeContractTestFramework::make_native_contract(0);
  fw.write_contract(deployer_contract);
  fw.write_deployment(kDeployerAddr, deployer_contract);
  fw.setup_grant_storage_token(kDeployerAddr, 1'000'000'000);
  fw.setup_grant_storage_token(deployed_addr, 1'000'000'000);

  BlockID old_block = fw.get_block_number();

  {
    auto handler = fw.make_new_system_call_handler(
        Transaction{
            .invocation =
                Invocation{.invoked_address = kDeployerAddr,
                           .entrypoint = entrypoint_from_method_id(0),
                           .gas_limit = UINT64_MAX,
                           .calldata = {}},
            .contracts_to_deploy = {contract_to_deploy},
        },
        context);

    auto res = handler.root_invoke();
    ASSERT_TRUE(res.is_root_success());

    auto commit_res = handler.try_commit_transaction();
    EXPECT_EQ(commit_res, ExecResult::ExecResultUnrecoverable);
  }

  x23::default_max_message_size = saved_max;

  // Discard the block. If the InvokeTX was properly aborted, the BlockTX
  // refcount is zero and the block can be cleaned up. If it leaked, the
  // BlockTX stays alive.
  fw.discard_block();

  // Probe the old block ID with a read. If the BlockTX was garbage
  // collected (InvokeTX properly closed), this returns Abandon. If the
  // leaked InvokeTX kept the BlockTX alive, this succeeds.
  auto tip_res = fw.get_db_proxy().get_db_tip(old_block);
  EXPECT_FALSE(tip_res.has_value())
      << "BlockTX should be gone after discard — leaked InvokeTX kept it alive";
}

} // namespace hogd
