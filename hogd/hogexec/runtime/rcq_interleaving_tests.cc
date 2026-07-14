// Model-checker-based interleaving tests for RCQ validation mode.
//
// For every proposal interleaving, enumerate ALL validation interleavings
// and assert: same commit/abort set.  State agreement (hash match) is
// covered by the ProposeValidateRoundtripTest tier.

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <span>
#include <unordered_map>
#include <utility>
#include <vector>

#include "exec_result.hh"
#include "ffi_enums.hh"
#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "hogd/hogexec/common/status.h"
#include "hogd/hogexec/runtime/contract_trait.h"
#include "hogd/hogexec/runtime/native_contract_test_framework.h"
#include "hogd/hogexec/runtime/native_runtime.h"
#include "hogd/hogexec/runtime/native_syscall_context.h"
#include "hogd/hogexec/runtime/runtime.h"
#include "hogd/hogexec/runtime/syscall_handler.h"
#include "model_checker/async.h"
#include "model_checker/work_queue.h"
#include "rpc_types.hh"
#include "storage_item.hh"
#include "transaction.hh"
#include "types.hh"

namespace hogd {

class RCQInterleavingTest : public ::testing::Test,
                            public NativeContractTestFramework {
public:
  RCQInterleavingTest()
    : NativeContractTestFramework(
          false, BlockExecutionMode::kBlockExecutionModeProposal,
          TestDB::kHogDB)
  {}

protected:
  static constexpr ContractAddress kAddr = make_contract_address(0);

  // Per-TX state tracked across proposal and validation.
  struct TxSlot {
    SignedTransaction tx;
    bool committed = true;
    NondeterministicProposalData proposal;
  };

  void SetUp() override
  {
    auto contract_xdr = make_native_contract(0);
    write_contract(contract_xdr);
    write_deployment(kAddr, contract_xdr);
    setup_grant_storage_token(kAddr, 1'000'000'000);

    // Calldata-driven RCQ contract:
    //   method 0: each calldata byte → one queue entry
    //   method 1: consume calldata[0] entries (default 1)
    std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns;
    fns.emplace(0,
                [](NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
                  auto const &cd = ctx.handler().get_calldata();
                  auto key = make_storage_key("rcq0");
                  for (uint8_t val : cd) {
                    auto res = ctx.handler().retrocausal_queue_insert(
                        key, std::as_bytes(std::span(&val, 1)));
                    if (!res.has_value())
                      return res.error().to_invoke_error();
                  }
                  return {};
                });
    fns.emplace(1,
                [](NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
                  auto const &cd = ctx.handler().get_calldata();
                  uint8_t count = cd.empty() ? 1 : cd[0];
                  auto key = make_storage_key("rcq0");
                  for (uint8_t i = 0; i < count; i++) {
                    auto res =
                        ctx.handler().retrocausal_queue_consume(key, false);
                    if (!res.has_value())
                      return res.error().to_invoke_error();
                  }
                  return {};
                });
    // method 2: delete the RCQ key
    fns.emplace(2,
                [](NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
                  auto key = make_storage_key("rcq0");
                  auto res = ctx.handler().delete_object(
                      key, std::to_underlying(StorageDeltaFlags::None));
                  if (!res.has_value())
                    return res.error().to_invoke_error();
                  return {};
                });
    // method 3: set bytestring (type change from RCQ)
    fns.emplace(3,
                [](NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
                  auto const &cd = ctx.handler().get_calldata();
                  auto key = make_storage_key("rcq0");
                  auto res = ctx.handler().set_bytestring(
                      key, std::as_bytes(std::span(cd)),
                      std::to_underlying(StorageDeltaFlags::None));
                  if (!res.has_value())
                    return res.error().to_invoke_error();
                  return {};
                });
    // method 4: delete + recreate (delete then insert)
    fns.emplace(4,
                [](NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
                  auto const &cd = ctx.handler().get_calldata();
                  auto key = make_storage_key("rcq0");
                  auto del = ctx.handler().delete_object(
                      key, std::to_underlying(StorageDeltaFlags::None));
                  if (!del.has_value())
                    return del.error().to_invoke_error();
                  if (!del.value().has_value())
                    return InvokeError{InvokeErrorCode::Deterministic};
                  for (uint8_t val : cd) {
                    auto ins = ctx.handler().retrocausal_queue_insert(
                        key, std::as_bytes(std::span(&val, 1)));
                    if (!ins.has_value())
                      return ins.error().to_invoke_error();
                  }
                  return {};
                });
    auto native_contract = std::make_unique<NativeContract>(std::move(fns));
    std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
    contract_map.emplace(0, std::move(native_contract));
    context_ = std::make_unique<NativeContext>(std::move(contract_map));
  }

  SignedTransaction make_tx(uint32_t method,
                            std::vector<uint8_t> const &calldata)
  {
    return SignedTransaction{
        .tx = Transaction{.invocation =
                              Invocation{.invoked_address = kAddr,
                                         .entrypoint =
                                             entrypoint_from_method_id(method),
                                         .gas_limit = UINT64_MAX,
                                         .calldata = Calldata(calldata.begin(),
                                                              calldata.end())},
                          .uniquifier = make_new_uniquifier()}};
  }

  SignedTransaction make_insert_tx(std::vector<uint8_t> const &values)
  {
    return make_tx(0, values);
  }

  SignedTransaction make_consume_tx(uint8_t count)
  {
    return make_tx(1, {count});
  }

  SignedTransaction make_delete_tx() { return make_tx(2, {}); }

  SignedTransaction make_bytestring_tx(std::vector<uint8_t> const &value)
  {
    return make_tx(3, value);
  }

  SignedTransaction make_delete_recreate_tx(std::vector<uint8_t> const &values)
  {
    return make_tx(4, values);
  }

  // --- Interleaving infrastructure ---

  template<bool Proposal>
  void add_tx(model::RunnableActionSet &actions, TxSlot &slot)
  {
    actions.add_action(
        [](model::RunnableActionSet &set, auto *self,
           TxSlot &s) -> model::Async {
          co_await set.bg();
          auto handler = [&]() {
            if constexpr (Proposal)
              return self->make_new_system_call_handler(s.tx, *self->context_);
            else
              return self->make_new_system_call_handler_validation(
                  s.tx.tx, *self->context_, s.proposal);
          }();
          auto res = handler.root_invoke();
          if (!res.is_root_success()) {
            s.committed = false;
            co_return;
          }
          if constexpr (Proposal)
            s.proposal = handler.get_proposal_data();
          auto commit_res = handler.try_commit_transaction();
          if (commit_res != ExecResult::ExecResultOK &&
              commit_res != ExecResult::ExecResultTxNonDeterministicError) {
            panic("unexpected commit error");
          }
          if (commit_res == ExecResult::ExecResultTxNonDeterministicError) {
            s.committed = false;
          }
          co_return;
        },
        this, slot);
  }

  void add_proposal_tx(model::RunnableActionSet &actions, TxSlot &slot)
  {
    add_tx<true>(actions, slot);
  }

  void add_validation_tx(model::RunnableActionSet &actions, TxSlot &slot)
  {
    add_tx<false>(actions, slot);
  }

  // Exhaustively enumerate proposal and validation interleavings,
  // asserting commit/abort agreement for every combination.
  void check_interleavings(std::vector<TxSlot> slots)
  {
    model::WorkQueue wq;
    while (!wq.done()) {
      advance_block();

      for (auto &s : slots)
        s.committed = true;
      model::RunnableActionSet actions(wq);
      for (auto &s : slots)
        add_proposal_tx(actions, s);
      ASSERT_EQ(actions.run(), model::ActionResult::kOk);

      // Snapshot proposal outcomes before discarding the block.
      std::vector<bool> proposed;
      proposed.reserve(slots.size());
      for (auto const &s : slots)
        proposed.push_back(s.committed);

      discard_block(BlockExecutionMode::kBlockExecutionModeValidation);

      model::WorkQueue val_wq;
      while (!val_wq.done()) {
        // Build validation slots only for committed TXs.
        std::vector<TxSlot> val_slots;
        val_slots.reserve(slots.size());
        for (size_t i = 0; i < slots.size(); i++) {
          if (proposed[i])
            val_slots.push_back({slots[i].tx, true, slots[i].proposal});
        }

        model::RunnableActionSet val_actions(val_wq);
        for (auto &s : val_slots)
          add_validation_tx(val_actions, s);
        ASSERT_EQ(val_actions.run(), model::ActionResult::kOk);

        for (size_t i = 0, vi = 0; i < slots.size(); i++) {
          if (proposed[i]) {
            EXPECT_EQ(val_slots[vi].committed, proposed[i])
                << "Commit/abort disagreement for TX " << i;
            vi++;
          }
        }

        discard_block(BlockExecutionMode::kBlockExecutionModeValidation);
        val_wq.advance_cursor();
      }

      wq.advance_cursor();
    }
  }

  std::unique_ptr<RuntimeContext> context_;
};

// Two concurrent inserters.  Insert-insert never conflicts (no
// nondeterministic bits), so both always commit.  Baseline sanity
// check for the interleaving framework on RCQ.
TEST_F(RCQInterleavingTest, TwoInserters)
{
  check_interleavings({
      {make_insert_tx({10})},
      {make_insert_tx({20})},
  });
}

// Inserter + concurrent consumer.  Exercises consumed_but_not_inserted
// reconciliation when validation commits the consumer before its
// corresponding inserter.
TEST_F(RCQInterleavingTest, InserterAndConcurrentConsumer)
{
  check_interleavings({
      {make_insert_tx({10})},
      {make_consume_tx(1)},
  });
}

// Two inserters + one consumer.  Multi-source concurrent entry
// reconciliation across all 3-TX commit orderings.
TEST_F(RCQInterleavingTest, TwoInsertersOneConsumer)
{
  check_interleavings({
      {make_insert_tx({10})},
      {make_insert_tx({20})},
      {make_consume_tx(2)},
  });
}

// Same topology as TwoInsertersOneConsumer but with identical entry
// values.  Targets L13 audit finding: consumed_but_not_inserted must
// use multiset (not set) semantics to handle duplicate values.
TEST_F(RCQInterleavingTest, DuplicateEntryValues)
{
  check_interleavings({
      {make_insert_tx({10})},
      {make_insert_tx({10})},
      {make_consume_tx(2)},
  });
}

// Insert + delete.  Tests whether delete always wins and whether
// insert conflicts with a concurrent delete.  The model checker
// enumerates all orderings so we verify both insert-before-delete
// and delete-before-insert commit sets match across proposal/validation.
TEST_F(RCQInterleavingTest, InsertAndConcurrentDelete)
{
  // Seed the RCQ with an entry so delete has something to act on.
  auto handler = make_new_system_call_handler(make_insert_tx({42}), *context_);
  ASSERT_TRUE(handler.root_invoke().is_root_success());
  ASSERT_EQ(handler.try_commit_transaction(), ExecResult::ExecResultOK);
  advance_block();

  check_interleavings({
      {make_insert_tx({10})},
      {make_delete_tx()},
  });
}

// Delete+recreate + concurrent insert.  One TX deletes the RCQ and
// immediately recreates it with new entries.  A concurrent TX inserts
// independently.  Verifies proposal/validation agreement under all
// orderings of these two TXs.
TEST_F(RCQInterleavingTest, DeleteRecreateAndConcurrentInsert)
{
  // Seed the RCQ so the delete+recreate has something to delete.
  auto handler = make_new_system_call_handler(make_insert_tx({42}), *context_);
  ASSERT_TRUE(handler.root_invoke().is_root_success());
  ASSERT_EQ(handler.try_commit_transaction(), ExecResult::ExecResultOK);
  advance_block();

  check_interleavings({
      {make_delete_recreate_tx({50, 60})},
      {make_insert_tx({10})},
  });
}

// Type-change (bytestring set) + concurrent RCQ insert.  One TX
// overwrites the RCQ with a bytestring; the other inserts into it.
// Exercises type-transition conflict detection across all orderings.
TEST_F(RCQInterleavingTest, TypeChangeAndConcurrentInsert)
{
  // Seed the RCQ.
  auto handler = make_new_system_call_handler(make_insert_tx({42}), *context_);
  ASSERT_TRUE(handler.root_invoke().is_root_success());
  ASSERT_EQ(handler.try_commit_transaction(), ExecResult::ExecResultOK);
  advance_block();

  check_interleavings({
      {make_bytestring_tx({0xAB, 0xCD})},
      {make_insert_tx({10})},
  });
}

} // namespace hogd
