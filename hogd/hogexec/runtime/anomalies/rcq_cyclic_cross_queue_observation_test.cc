// Tolerated concurrency anomaly: cyclic cross-queue RCQ observations.
//
// Two transactions each consume from one RCQ key and insert into the other.
// No linearizable execution order exists: TX_A needs TX_B's insert to
// observe, and TX_B needs TX_A's insert.  But crafted nondeterministic
// proposal data can make both succeed because value-based reconciliation
// only checks that each observed value was inserted somewhere in the block —
// it does not enforce a causal ordering between transactions.
//
// Why this is tolerated:
//   - The RCQ invariant only requires that every observed value is inserted
//     by some committing transaction in the same block.  Both values are
//     inserted, and both observations match.
//   - Requiring linearizability would prevent valid concurrent insert/consume
//     patterns that arise naturally when transactions interact through queues.
//   - The propose/validate split intentionally decouples observation from
//     execution order: the proposer records what each transaction saw, and
//     validators only check that the observations are consistent with the
//     block's total set of inserts.
//
// See:
//   - https://github.com/groundhogchain/hogd/issues/2359
//   - https://github.com/groundhogchain/hogd/issues/2381
//   - https://github.com/groundhogchain/hogd/issues/2382

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "exec_result.hh"
#include "ffi_enums.hh"
#include "hogd/base/types.h"
#include "hogd/hogexec/common/status.h"
#include "hogd/hogexec/runtime/contract_trait.h"
#include "hogd/hogexec/runtime/native_contract_test_framework.h"
#include "hogd/hogexec/runtime/native_runtime.h"
#include "hogd/hogexec/runtime/native_syscall_context.h"
#include "hogd/hogexec/runtime/runtime.h"
#include "hogd/hogexec/runtime/syscall_handler.h"
#include "hogd/hogexec/runtime/syscall_inner.h"
#include "retrocausal_queue.hh"
#include "rpc_types.hh"
#include "storage_item.hh"
#include "syscall_error.hh"
#include "transaction.hh"
#include "types.hh"

namespace hogd {

// NOLINTBEGIN(bugprone-exception-escape,bugprone-unchecked-optional-access)

// NOLINTNEXTLINE(misc-use-internal-linkage)
class RCQCyclicCrossQueueTest : public ::testing::Test,
                                public NativeContractTestFramework {
public:
  RCQCyclicCrossQueueTest()
    : NativeContractTestFramework(
          false, BlockExecutionMode::kBlockExecutionModeProposal,
          TestDB::kHogDB)
  {}

  void SetUp() override
  {
    auto contract_xdr = make_native_contract(0);
    write_contract(contract_xdr);
    write_deployment(kAddr, contract_xdr);
    setup_grant_storage_token(kAddr, 1'000'000'000);
  }

  static constexpr ContractAddress kAddr = make_contract_address(0);

  template<typename... Fns>
  static std::unique_ptr<NativeContract> make_contract(Fns &&...fns)
  {
    std::unordered_map<Runtime::MethodID, NativeContract::function_t> map;
    Runtime::MethodID id = 0;
    (map.emplace(id++, std::forward<Fns>(fns)), ...);
    return std::make_unique<NativeContract>(std::move(map));
  }

  Transaction make_tx(uint32_t method,
                      std::vector<uint8_t> const &calldata = {})
  {
    return Transaction{
        .invocation =
            Invocation{.invoked_address = kAddr,
                       .entrypoint = entrypoint_from_method_id(method),
                       .gas_limit = UINT64_MAX,
                       .calldata = Calldata(calldata.begin(), calldata.end())},
        .uniquifier = make_new_uniquifier()};
  }
};

// TX_A: peek queue_a → observe {0xAA}, insert {0xBB} into queue_b.
// TX_B: peek queue_b → observe {0xBB}, insert {0xAA} into queue_a.
//
// No linearizable order works:
//   - If TX_A runs first, queue_a is empty → TX_A's peek fails.
//   - If TX_B runs first, queue_b is empty → TX_B's peek fails.
//
// With crafted ndet data, both succeed: each observation is matched by the
// other transaction's insert.
TEST_F(RCQCyclicCrossQueueTest, CyclicCrossQueueObservationsSucceed)
{
  static constexpr uint8_t kValA = 0xAA;
  static constexpr uint8_t kValB = 0xBB;

  auto contract = make_contract(
      // method 0 (TX_A): peek queue_a, require {kValA}, insert {kValB} into
      // queue_b
      [](NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
        auto key_a = make_storage_key("queue_a");
        auto peek_res = ctx.handler().retrocausal_queue_consume(key_a, true);
        if (!peek_res.has_value())
          return peek_res.error().to_invoke_error();
        if (!peek_res.value().has_value())
          return InvokeError{InvokeErrorCode::Deterministic};

        auto const &entry = peek_res.value().value();
        if (entry.size() != 1 || entry[0] != kValA)
          return InvokeError{InvokeErrorCode::Deterministic};

        uint8_t val = kValB;
        auto key_b = make_storage_key("queue_b");
        auto ins = ctx.handler().retrocausal_queue_insert(
            key_b, std::as_bytes(std::span(&val, 1)));
        if (!ins.has_value())
          return ins.error().to_invoke_error();

        auto [ws, _wg] = syscall_BytestringSet_inner(
            ctx.handler(), make_storage_key("marker_a"),
            std::as_bytes(std::span(std::string_view("a_ok"))),
            std::to_underlying(StorageDeltaFlags::None));
        if (!ws.has_value() ||
            ws.value() != SyscallErrorCode::kSyscallErrorCodeNone)
          return InvokeError{InvokeErrorCode::Deterministic};
        return {};
      },
      // method 1 (TX_B): peek queue_b, require {kValB}, insert {kValA} into
      // queue_a
      [](NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
        auto key_b = make_storage_key("queue_b");
        auto peek_res = ctx.handler().retrocausal_queue_consume(key_b, true);
        if (!peek_res.has_value())
          return peek_res.error().to_invoke_error();
        if (!peek_res.value().has_value())
          return InvokeError{InvokeErrorCode::Deterministic};

        auto const &entry = peek_res.value().value();
        if (entry.size() != 1 || entry[0] != kValB)
          return InvokeError{InvokeErrorCode::Deterministic};

        uint8_t val = kValA;
        auto key_a = make_storage_key("queue_a");
        auto ins = ctx.handler().retrocausal_queue_insert(
            key_a, std::as_bytes(std::span(&val, 1)));
        if (!ins.has_value())
          return ins.error().to_invoke_error();

        auto [ws, _wg] = syscall_BytestringSet_inner(
            ctx.handler(), make_storage_key("marker_b"),
            std::as_bytes(std::span(std::string_view("b_ok"))),
            std::to_underlying(StorageDeltaFlags::None));
        if (!ws.has_value() ||
            ws.value() != SyscallErrorCode::kSyscallErrorCodeNone)
          return InvokeError{InvokeErrorCode::Deterministic};
        return {};
      });

  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> cmap;
  cmap.emplace(0, std::move(contract));
  auto ctx = std::make_unique<NativeContext>(std::move(cmap));

  auto tx_a = make_tx(0);
  auto tx_b = make_tx(1);

  // Honest proposal: neither order works.
  {
    auto ha = make_new_system_call_handler(tx_a, *ctx);
    ASSERT_FALSE(ha.root_invoke().is_root_success())
        << "TX_A fails: queue_a is empty, nothing to peek";
    ASSERT_TRUE(ha.abort_transaction().has_value());

    auto hb = make_new_system_call_handler(tx_b, *ctx);
    ASSERT_FALSE(hb.root_invoke().is_root_success())
        << "TX_B fails: queue_b is empty, nothing to peek";
    ASSERT_TRUE(hb.abort_transaction().has_value());
  }

  EXPECT_FALSE(
      lookup_storage_item_from_db(make_contract_data_addrkey(kAddr, "marker_a"))
          .has_value());
  EXPECT_FALSE(
      lookup_storage_item_from_db(make_contract_data_addrkey(kAddr, "marker_b"))
          .has_value());

  // Forge ndet data for both TXs.
  NondeterministicProposalData forged_a;
  forged_a.nondeterministic_bit_count = 1;
  forged_a.nondeterministic_bits = {uint8_t{1}};
  forged_a.observed_queue_entry_sequence = {QueueEntry{kValA}};

  NondeterministicProposalData forged_b;
  forged_b.nondeterministic_bit_count = 1;
  forged_b.nondeterministic_bits = {uint8_t{1}};
  forged_b.observed_queue_entry_sequence = {QueueEntry{kValB}};

  discard_block(BlockExecutionMode::kBlockExecutionModeValidation);

  {
    auto ha = make_new_system_call_handler_validation(tx_a, *ctx,
                                                      std::move(forged_a));
    ASSERT_TRUE(ha.root_invoke().is_root_success());
    ASSERT_EQ(ha.try_commit_transaction(), ExecResult::ExecResultOK);

    auto hb = make_new_system_call_handler_validation(tx_b, *ctx,
                                                      std::move(forged_b));
    ASSERT_TRUE(hb.root_invoke().is_root_success());
    ASSERT_EQ(hb.try_commit_transaction(), ExecResult::ExecResultOK);
  }

  ASSERT_NO_THROW(
      advance_block(BlockExecutionMode::kBlockExecutionModeValidation))
      << "block closes: queue_a has {kValA} inserted by TX_B, queue_b has "
         "{kValB} inserted by TX_A — each observation is matched";

  auto marker_a = lookup_storage_item_from_db(
      make_contract_data_addrkey(kAddr, "marker_a"));
  ASSERT_TRUE(marker_a.has_value());
  EXPECT_EQ(marker_a->bytestring(), make_bytestring("a_ok"));

  auto marker_b = lookup_storage_item_from_db(
      make_contract_data_addrkey(kAddr, "marker_b"));
  ASSERT_TRUE(marker_b.has_value());
  EXPECT_EQ(marker_b->bytestring(), make_bytestring("b_ok"));
}

// NOLINTEND(bugprone-exception-escape,bugprone-unchecked-optional-access)

} // namespace hogd
