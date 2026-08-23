// Tolerated concurrency anomaly: RCQ observations are validated by value,
// not by index or temporal ordering.
//
// The RCQ invariant is:
//   If you observe a value from the current-block queue, that value must be
//   inserted in the same block by some transaction that commits.
//
// What is explicitly *not* required:
//   - The observation need not match the entry at any particular index.
//   - The observer need not execute after the inserter (not linearizable).
//   - A self-insert can satisfy a self-observation.
//
// Why this is tolerated:
//   - Current-block RCQ entry ordering is nondeterministic across proposers.
//     Entries are inserted by concurrent transactions whose execution order
//     varies per node.  There is no canonical index assignment.
//   - Value-based reconciliation (observe_count <= insert_count per value)
//     correctly enforces the invariant above.
//   - Index-binding or temporal-ordering constraints would over-constrain the
//     system by requiring validators to reproduce one proposer's scheduling
//     order.
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
#include "hogd/hog_exec/common/status.h"
#include "hogd/hog_exec/runtime/contract_trait.h"
#include "hogd/hog_exec/runtime/native_contract_test_framework.h"
#include "hogd/hog_exec/runtime/native_runtime.h"
#include "hogd/hog_exec/runtime/native_syscall_context.h"
#include "hogd/hog_exec/runtime/runtime.h"
#include "hogd/hog_exec/runtime/syscall_handler.h"
#include "hogd/hog_exec/runtime/syscall_inner.h"
#include "retrocausal_queue.hh"
#include "rpc_types.hh"
#include "storage_item.hh"
#include "syscall_error.hh"
#include "transaction.hh"
#include "types.hh"

namespace hogd {

// NOLINTBEGIN(bugprone-exception-escape,bugprone-unchecked-optional-access)

// NOLINTNEXTLINE(misc-use-internal-linkage)
class RCQObservationByValueTest : public ::testing::Test,
                                  public NativeContractTestFramework {
public:
  RCQObservationByValueTest()
    : NativeContractTestFramework(
          false, BlockExecutionMode::kBlockExecutionModeProposal,
          TestDB::kHogDb)
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

// Issue #2381: Read-only peek observation swapped to another inserted value.
//
// Two sibling TXs insert {10} then {20} into the same RCQ key.  A third TX
// peeks (read-only) at cursor index 0, which honestly returns {10}.  When
// validation replays with the peek observation swapped to {20}, the block
// still closes successfully.
//
// WAI: a different proposer could legitimately insert {20} before {10},
// making index 0 return {20} honestly.
TEST_F(RCQObservationByValueTest, PeekObservationSwappedToAnotherInsertedValue)
{
  std::vector<uint8_t> observed_values;

  auto contract = make_contract(
      [](NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
        auto const &cd = ctx.handler().get_calldata();
        auto key = make_storage_key("rcq0");
        for (uint8_t val : cd) {
          auto res = ctx.handler().retrocausal_queue_insert(
              key, std::as_bytes(std::span(&val, 1)))();
          if (!res.has_value())
            return res.error().to_invoke_error();
        }
        return {};
      },
      [&observed_values](
          NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
        auto key = make_storage_key("rcq0");
        auto peek_res = ctx.handler().retrocausal_queue_consume(key, true)();
        if (!peek_res.has_value())
          return peek_res.error().to_invoke_error();
        if (!peek_res.value().has_value())
          return InvokeError{InvokeErrorCode::Deterministic};

        QueueEntry const &entry = peek_res.value().value();
        if (entry.size() != 1)
          return InvokeError{InvokeErrorCode::Deterministic};
        observed_values.push_back(entry[0]);

        auto [ws, _wg] = syscall_BytestringSet_inner(
            ctx.handler(), make_storage_key("result"),
            std::as_bytes(std::span(entry.data(), entry.size())),
            std::to_underlying(StorageDeltaFlags::None));
        if (!ws.has_value() ||
            ws.value() != SyscallErrorCode::kSyscallErrorCodeNone)
          return InvokeError{InvokeErrorCode::Deterministic};
        return {};
      });

  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> cmap;
  cmap.emplace(0, std::move(contract));
  auto ctx = std::make_unique<NativeContext>(std::move(cmap));

  auto tx_insert_10 = make_tx(0, {10});
  auto tx_insert_20 = make_tx(0, {20});
  auto tx_peek = make_tx(1);

  NondeterministicProposalData insert_10_p, insert_20_p, peek_p;
  {
    auto h10 = make_new_system_call_handler(tx_insert_10, *ctx);
    ASSERT_TRUE(h10.root_invoke()().is_root_success());
    insert_10_p = h10.get_proposal_data();
    ASSERT_EQ(h10.try_commit_transaction()(), ExecResult::ExecResultOK);

    auto h20 = make_new_system_call_handler(tx_insert_20, *ctx);
    ASSERT_TRUE(h20.root_invoke()().is_root_success());
    insert_20_p = h20.get_proposal_data();
    ASSERT_EQ(h20.try_commit_transaction()(), ExecResult::ExecResultOK);

    auto hpeek = make_new_system_call_handler(tx_peek, *ctx);
    ASSERT_TRUE(hpeek.root_invoke()().is_root_success());
    peek_p = hpeek.get_proposal_data();
    ASSERT_EQ(hpeek.try_commit_transaction()(), ExecResult::ExecResultOK);
  }

  ASSERT_EQ(observed_values, (std::vector<uint8_t>{10}));
  ASSERT_EQ(peek_p.observed_queue_entry_sequence.size(), 1U);
  ASSERT_EQ(peek_p.observed_queue_entry_sequence[0], QueueEntry{10});

  auto swapped_peek_p = peek_p;
  swapped_peek_p.observed_queue_entry_sequence[0] = QueueEntry{20};

  observed_values.clear();
  discard_block(BlockExecutionMode::kBlockExecutionModeValidation);

  {
    auto h10 = make_new_system_call_handler_validation(tx_insert_10, *ctx,
                                                       insert_10_p);
    ASSERT_TRUE(h10.root_invoke()().is_root_success());
    ASSERT_EQ(h10.try_commit_transaction()(), ExecResult::ExecResultOK);

    auto h20 = make_new_system_call_handler_validation(tx_insert_20, *ctx,
                                                       insert_20_p);
    ASSERT_TRUE(h20.root_invoke()().is_root_success());
    ASSERT_EQ(h20.try_commit_transaction()(), ExecResult::ExecResultOK);

    auto hpeek =
        make_new_system_call_handler_validation(tx_peek, *ctx,
                                                std::move(swapped_peek_p));
    ASSERT_TRUE(hpeek.root_invoke()().is_root_success());
    ASSERT_EQ(hpeek.try_commit_transaction()(), ExecResult::ExecResultOK);
  }

  EXPECT_EQ(observed_values, (std::vector<uint8_t>{20}))
      << "known anomaly: validation replays the proposer-supplied observation "
         "for a peek, not the live index value (issue #2381)";
  ASSERT_NO_THROW(
      advance_block(BlockExecutionMode::kBlockExecutionModeValidation))
      << "value-based reconciliation accepts the swapped observation because "
         "{20} was inserted in the same block";

  auto result =
      lookup_storage_item_from_db(make_contract_data_addrkey(kAddr, "result"));
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->bytestring(), (Bytestring{20}));
}

// Issue #2382: Destructive consume observation swapped to another inserted
// value.
//
// TX_A inserts {10, 20} into the same RCQ key.  TX_B destructively consumes
// one entry, which honestly returns {10} (cursor index 0).  When validation
// replays with the consume observation swapped to {20}, the block still
// closes: {20} was inserted in the same block, and value-based accounting
// treats the consume of {20} as matched.
//
// WAI: same reasoning as the peek case — a different proposer could order the
// inserts differently, making cursor index 0 yield {20} honestly.
TEST_F(RCQObservationByValueTest,
       DestructiveConsumeSwappedToAnotherInsertedValue)
{
  std::vector<uint8_t> consumed_values;

  auto contract = make_contract(
      [](NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
        auto const &cd = ctx.handler().get_calldata();
        auto key = make_storage_key("rcq0");
        for (uint8_t val : cd) {
          auto res = ctx.handler().retrocausal_queue_insert(
              key, std::as_bytes(std::span(&val, 1)))();
          if (!res.has_value())
            return res.error().to_invoke_error();
        }
        return {};
      },
      [&consumed_values](
          NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
        auto key = make_storage_key("rcq0");
        auto consume_res = ctx.handler().retrocausal_queue_consume(key, false)();
        if (!consume_res.has_value())
          return consume_res.error().to_invoke_error();
        if (!consume_res.value().has_value())
          return InvokeError{InvokeErrorCode::Deterministic};

        auto const &entry = consume_res.value().value();
        if (entry.size() != 1)
          return InvokeError{InvokeErrorCode::Deterministic};
        consumed_values.push_back(entry[0]);

        auto [ws, _wg] = syscall_BytestringSet_inner(
            ctx.handler(), make_storage_key("result"),
            std::as_bytes(std::span(entry.data(), entry.size())),
            std::to_underlying(StorageDeltaFlags::None));
        if (!ws.has_value() ||
            ws.value() != SyscallErrorCode::kSyscallErrorCodeNone)
          return InvokeError{InvokeErrorCode::Deterministic};
        return {};
      });

  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> cmap;
  cmap.emplace(0, std::move(contract));
  auto ctx = std::make_unique<NativeContext>(std::move(cmap));

  auto tx_insert = make_tx(0, {10, 20});
  auto tx_consume = make_tx(1);

  NondeterministicProposalData insert_p, consume_p;
  {
    auto ha = make_new_system_call_handler(tx_insert, *ctx);
    ASSERT_TRUE(ha.root_invoke()().is_root_success());
    insert_p = ha.get_proposal_data();
    ASSERT_EQ(ha.try_commit_transaction()(), ExecResult::ExecResultOK);

    auto hb = make_new_system_call_handler(tx_consume, *ctx);
    ASSERT_TRUE(hb.root_invoke()().is_root_success());
    consume_p = hb.get_proposal_data();
    ASSERT_EQ(consume_p.observed_queue_entry_sequence.size(), 1U);
    EXPECT_EQ(consume_p.observed_queue_entry_sequence[0], QueueEntry{10});
    ASSERT_EQ(hb.try_commit_transaction()(), ExecResult::ExecResultOK);
  }

  ASSERT_EQ(consumed_values, (std::vector<uint8_t>{10}));

  auto swapped_consume_p = consume_p;
  swapped_consume_p.observed_queue_entry_sequence[0] = QueueEntry{20};

  consumed_values.clear();
  discard_block(BlockExecutionMode::kBlockExecutionModeValidation);

  {
    auto ha =
        make_new_system_call_handler_validation(tx_insert, *ctx, insert_p);
    ASSERT_TRUE(ha.root_invoke()().is_root_success());
    ASSERT_EQ(ha.try_commit_transaction()(), ExecResult::ExecResultOK);

    auto hb =
        make_new_system_call_handler_validation(tx_consume, *ctx,
                                                std::move(swapped_consume_p));
    ASSERT_TRUE(hb.root_invoke()().is_root_success());
    ASSERT_EQ(hb.try_commit_transaction()(), ExecResult::ExecResultOK);
  }

  EXPECT_EQ(consumed_values, (std::vector<uint8_t>{20}))
      << "known anomaly: validation replays the proposer-supplied observation "
         "for a destructive consume, not the live index value (issue #2382)";
  ASSERT_NO_THROW(
      advance_block(BlockExecutionMode::kBlockExecutionModeValidation))
      << "value-based reconciliation accepts the swapped consume because "
         "{20} was inserted in the same block";

  auto result =
      lookup_storage_item_from_db(make_contract_data_addrkey(kAddr, "result"));
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->bytestring(), (Bytestring{20}));
}

// Issue #2359: Forged peek satisfied by a later self-insert.
//
// A contract peeks an empty RCQ key, requires a specific value, then inserts
// that same value.  Honest proposal on the empty queue sees no entry and
// fails.  Validation with forged ndet data (bit=true, observation={0xA7})
// succeeds: the contract sees the forged entry, inserts the same bytes, and
// the later self-insert satisfies the observation in value-based accounting.
//
// WAI: the RCQ invariant only requires that the observed value is inserted
// *somewhere in the block* by a committing TX — it does not require temporal
// ordering.  The observer and inserter can be the same transaction.
TEST_F(RCQObservationByValueTest, ForgedPeekSatisfiedByLaterSelfInsert)
{
  static constexpr uint8_t kEntry = 0xA7;

  auto contract = make_contract(
      [](NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
        auto key = make_storage_key("rcq0");

        auto peek_res = ctx.handler().retrocausal_queue_consume(key, true)();
        if (!peek_res.has_value())
          return peek_res.error().to_invoke_error();
        if (!peek_res.value().has_value())
          return InvokeError{InvokeErrorCode::Deterministic};

        auto const &peeked = peek_res.value().value();
        if (peeked.size() != 1 || peeked[0] != kEntry)
          return InvokeError{InvokeErrorCode::Deterministic};

        uint8_t entry = kEntry;
        auto insert_res = ctx.handler().retrocausal_queue_insert(
            key, std::as_bytes(std::span(&entry, 1)))();
        if (!insert_res.has_value())
          return insert_res.error().to_invoke_error();

        auto marker = std::string_view("ok");
        auto [ws, _wg] = syscall_BytestringSet_inner(
            ctx.handler(), make_storage_key("marker"),
            std::as_bytes(std::span(marker)),
            std::to_underlying(StorageDeltaFlags::None));
        if (!ws.has_value() ||
            ws.value() != SyscallErrorCode::kSyscallErrorCodeNone)
          return InvokeError{InvokeErrorCode::Deterministic};
        return {};
      });

  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> cmap;
  cmap.emplace(0, std::move(contract));
  auto ctx = std::make_unique<NativeContext>(std::move(cmap));
  auto tx = make_tx(0);

  // Honest proposal: empty queue, peek sees nothing, contract fails.
  {
    auto handler = make_new_system_call_handler(tx, *ctx);
    auto res = handler.root_invoke()();
    ASSERT_FALSE(res.is_root_success())
        << "honest proposal on an empty RCQ cannot see the entry";
    auto honest_p = handler.get_proposal_data();
    ASSERT_EQ(honest_p.nondeterministic_bit_count, 1U);
    ASSERT_EQ(honest_p.observed_queue_entry_sequence.size(), 0U);
    EXPECT_EQ(honest_p.nondeterministic_bits[0] & uint8_t{1}, 0);
    ASSERT_TRUE(handler.abort_transaction()().has_value());
  }

  EXPECT_FALSE(
      lookup_storage_item_from_db(make_contract_data_addrkey(kAddr, "marker"))
          .has_value());

  // Forge ndet data: bit=true, observation={kEntry}.
  NondeterministicProposalData forged;
  forged.nondeterministic_bit_count = 1;
  forged.nondeterministic_bits = {uint8_t{1}};
  forged.observed_queue_entry_sequence = {QueueEntry{kEntry}};

  discard_block(BlockExecutionMode::kBlockExecutionModeValidation);

  auto handler =
      make_new_system_call_handler_validation(tx, *ctx, std::move(forged));
  ASSERT_TRUE(handler.root_invoke()().is_root_success());
  EXPECT_EQ(handler.try_commit_transaction()(), ExecResult::ExecResultOK)
      << "known anomaly: the later self-insert satisfies the earlier "
         "observation in value-based accounting (issue #2359)";
  ASSERT_NO_THROW(
      advance_block(BlockExecutionMode::kBlockExecutionModeValidation));

  auto marker =
      lookup_storage_item_from_db(make_contract_data_addrkey(kAddr, "marker"));
  ASSERT_TRUE(marker.has_value());
  EXPECT_EQ(marker->bytestring(), make_bytestring("ok"));
}

// Non-linearizable observation: a transaction observes a value that was
// inserted by a *later* transaction in the block.
//
// TX_A peeks at an empty RCQ and (with forged ndet data) observes {42}.
// TX_B, which executes after TX_A, inserts {42}.  The block closes because
// {42} was inserted by a committing TX in the same block.
//
// WAI: the RCQ invariant does not require linearizability — the observation
// only needs a matching insert somewhere in the block, regardless of
// transaction execution order.
TEST_F(RCQObservationByValueTest, ObserverSeesLaterInserterEntry)
{
  auto contract = make_contract(
      // method 0: peek and write what we see
      [](NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
        auto key = make_storage_key("rcq0");
        auto peek_res = ctx.handler().retrocausal_queue_consume(key, true)();
        if (!peek_res.has_value())
          return peek_res.error().to_invoke_error();
        if (!peek_res.value().has_value())
          return InvokeError{InvokeErrorCode::Deterministic};

        auto const &entry = peek_res.value().value();
        auto [ws, _wg] = syscall_BytestringSet_inner(
            ctx.handler(), make_storage_key("saw"),
            std::as_bytes(std::span(entry.data(), entry.size())),
            std::to_underlying(StorageDeltaFlags::None));
        if (!ws.has_value() ||
            ws.value() != SyscallErrorCode::kSyscallErrorCodeNone)
          return InvokeError{InvokeErrorCode::Deterministic};
        return {};
      },
      // method 1: insert calldata bytes as RCQ entries
      [](NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
        auto const &cd = ctx.handler().get_calldata();
        auto key = make_storage_key("rcq0");
        for (uint8_t val : cd) {
          auto res = ctx.handler().retrocausal_queue_insert(
              key, std::as_bytes(std::span(&val, 1)))();
          if (!res.has_value())
            return res.error().to_invoke_error();
        }
        return {};
      });

  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> cmap;
  cmap.emplace(0, std::move(contract));
  auto ctx = std::make_unique<NativeContext>(std::move(cmap));

  auto tx_peek = make_tx(0);
  auto tx_insert = make_tx(1, {42});

  // Honest proposal: TX_A peeks an empty queue, fails.
  {
    auto handler = make_new_system_call_handler(tx_peek, *ctx);
    ASSERT_FALSE(handler.root_invoke()().is_root_success());
    ASSERT_TRUE(handler.abort_transaction()().has_value());
  }

  // TX_B inserts {42} honestly.
  NondeterministicProposalData insert_p;
  {
    auto handler = make_new_system_call_handler(tx_insert, *ctx);
    ASSERT_TRUE(handler.root_invoke()().is_root_success());
    insert_p = handler.get_proposal_data();
    ASSERT_EQ(handler.try_commit_transaction()(), ExecResult::ExecResultOK);
  }

  // Forge TX_A's ndet data: bit=true, observation={42}.
  NondeterministicProposalData forged_peek;
  forged_peek.nondeterministic_bit_count = 1;
  forged_peek.nondeterministic_bits = {uint8_t{1}};
  forged_peek.observed_queue_entry_sequence = {QueueEntry{42}};

  discard_block(BlockExecutionMode::kBlockExecutionModeValidation);

  // Validation: TX_A runs first (observes {42}), TX_B runs second (inserts
  // {42}).  TX_A's observation is satisfied by TX_B's later insert.
  {
    auto ha = make_new_system_call_handler_validation(tx_peek, *ctx,
                                                      std::move(forged_peek));
    ASSERT_TRUE(ha.root_invoke()().is_root_success());
    ASSERT_EQ(ha.try_commit_transaction()(), ExecResult::ExecResultOK);

    auto hb =
        make_new_system_call_handler_validation(tx_insert, *ctx, insert_p);
    ASSERT_TRUE(hb.root_invoke()().is_root_success());
    ASSERT_EQ(hb.try_commit_transaction()(), ExecResult::ExecResultOK);
  }

  ASSERT_NO_THROW(
      advance_block(BlockExecutionMode::kBlockExecutionModeValidation))
      << "block closes because {42} was inserted by a committing TX, even "
         "though the insert happened after the observation";

  auto saw =
      lookup_storage_item_from_db(make_contract_data_addrkey(kAddr, "saw"));
  ASSERT_TRUE(saw.has_value());
  EXPECT_EQ(saw->bytestring(), (Bytestring{42}));
}

// NOLINTEND(bugprone-exception-escape,bugprone-unchecked-optional-access)

} // namespace hogd
