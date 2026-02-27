// -*-C++-*-

// Model-checker-based interleaving tests for RCQ validation mode.
//
// Pattern (from interleaving_tests.cc):
// 1. Outer WorkQueue explores proposal interleavings
// 2. Record commit/abort set, NondeterministicProposalData per TX,
//    and final DbTip.hash
// 3. Inner WorkQueue explores ALL validation interleavings
// 4. Assert: same commit/abort set AND same DbTip.hash

#include <gtest/gtest.h>

#include <cassert>
#include <cstdint>
#include <memory>
#include <span>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <vector>

#include "exec_result.hh"
#include "hogd/base/types.h"
#include "hogd/hogexec/runtime/contract_trait.h"
#include "hogd/hogexec/runtime/native_contract_test_framework.h"
#include "hogd/hogexec/runtime/native_runtime.h"
#include "hogd/hogexec/runtime/native_syscall_context.h"
#include "hogd/hogexec/runtime/runtime.h"
#include "hogd/hogexec/runtime/status.h"
#include "hogd/hogexec/runtime/syscall_handler.h"
#include "model_checker/async.h"
#include "model_checker/work_queue.h"
#include "rpc_types.hh"
#include "transaction.hh"
#include "types.hh"

namespace hogd {

// NOLINTBEGIN(bugprone-unchecked-optional-access)

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

  std::unique_ptr<RuntimeContext> make_rcq_context()
  {
    auto native_contract = std::make_unique<NativeContract>(
        std::unordered_map<Runtime::MethodID, NativeContract::function_t>{
            {0, // Insert: calldata bytes are entry values
             [](NativeSyscallContext &ctx) -> InvokeStatus<void> {
               auto const &calldata = ctx.handler().get_calldata();
               auto key = make_storage_key("rcq0");
               for (uint8_t val : calldata) {
                 auto res = ctx.handler().retrocausal_queue_insert(
                     key, std::as_bytes(std::span(&val, 1)));
                 if (!res.has_value()) {
                   return res.error().to_invoke_error();
                 }
               }
               return {};
             }},
            {1, // Consume: calldata[0] = count to consume
             [](NativeSyscallContext &ctx) -> InvokeStatus<void> {
               auto const &calldata = ctx.handler().get_calldata();
               uint8_t count = calldata.empty() ? 1 : calldata[0];
               auto key = make_storage_key("rcq0");
               for (uint8_t i = 0; i < count; i++) {
                 auto res =
                     ctx.handler().retrocausal_queue_consume(key, false);
                 if (!res.has_value()) {
                   return res.error().to_invoke_error();
                 }
               }
               return {};
             }},
        });

    std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
    contract_map.emplace(0, std::move(native_contract));
    return std::make_unique<NativeContext>(std::move(contract_map));
  }

  void SetUp() override
  {
    auto contract_xdr = make_native_contract(0);
    write_contract(contract_xdr);
    write_deployment(kAddr, contract_xdr);
    setup_grant_storage_token(kAddr, 1'000'000'000);
    context_ = make_rcq_context();
  }

  SignedTransaction make_insert_tx(std::vector<uint8_t> const &values)
  {
    return SignedTransaction{
        .tx = Transaction{
            .invocation = Invocation{
                .invoked_address = kAddr,
                .entrypoint = entrypoint_from_method_id(0),
                .gas_limit = UINT64_MAX,
                .calldata = Calldata(values.begin(), values.end())}}};
  }

  SignedTransaction make_consume_tx(uint8_t count)
  {
    return SignedTransaction{
        .tx = Transaction{
            .invocation = Invocation{
                .invoked_address = kAddr,
                .entrypoint = entrypoint_from_method_id(1),
                .gas_limit = UINT64_MAX,
                .calldata = Calldata({count})}}};
  }

  std::unique_ptr<RuntimeContext> context_;
};

// Scenario A: Two Concurrent Inserters.
// No nondeterministic RCQ bits (insert-insert never conflicts).
// Verifies commit/abort agreement regardless of commit order.
//
// Note: we discard both proposal and validation blocks (never commit them
// to the persistent DB) so that every iteration starts from the same base
// state.  Hash comparison is not possible without committing, but commit/
// abort agreement is the primary invariant for interleaving tests.  State
// agreement (hash match) is covered by the Tier 2 roundtrip tests.
TEST_F(RCQInterleavingTest, TwoInserters)
{
  auto insert_tx_a = make_insert_tx({10});
  auto insert_tx_b = make_insert_tx({20});

  model::WorkQueue work_queue;

  while (!work_queue.done()) {
    // Commit previous empty block, open fresh proposal block
    advance_block();

    model::RunnableActionSet actions(work_queue);

    bool a_ok = true;
    bool b_ok = true;
    NondeterministicProposalData a_proposal;
    NondeterministicProposalData b_proposal;

    actions.add_action(
        [](model::RunnableActionSet &set, auto *self, bool &ok,
           NondeterministicProposalData &proposal,
           SignedTransaction &tx) -> model::Async {
          co_await set.bg();
          auto handler =
              self->make_new_system_call_handler(tx, *self->context_);
          auto res = handler.root_invoke();
          if (!res.is_root_success()) {
            ok = false;
            co_return;
          }
          proposal = handler.get_proposal_data();
          auto commit_res = handler.try_commit_transaction();
          if (commit_res != ExecResult::ExecResultOK) {
            ok = false;
          }
          co_return;
        },
        this, a_ok, a_proposal, insert_tx_a);

    actions.add_action(
        [](model::RunnableActionSet &set, auto *self, bool &ok,
           NondeterministicProposalData &proposal,
           SignedTransaction &tx) -> model::Async {
          co_await set.bg();
          auto handler =
              self->make_new_system_call_handler(tx, *self->context_);
          auto res = handler.root_invoke();
          if (!res.is_root_success()) {
            ok = false;
            co_return;
          }
          proposal = handler.get_proposal_data();
          auto commit_res = handler.try_commit_transaction();
          if (commit_res != ExecResult::ExecResultOK) {
            ok = false;
          }
          co_return;
        },
        this, b_ok, b_proposal, insert_tx_b);

    ASSERT_EQ(actions.run(), model::ActionResult::kOk);
    ASSERT_TRUE(a_ok) << "Both inserters should always commit";
    ASSERT_TRUE(b_ok) << "Both inserters should always commit";

    // Discard proposal (don't commit to DB), open validation block
    // from the same pre-proposal base state.
    discard_block(BlockExecutionMode::kBlockExecutionModeValidation);

    // Validate: enumerate all validation interleavings
    model::WorkQueue validation_wq;
    while (!validation_wq.done()) {
      model::RunnableActionSet val_actions(validation_wq);
      bool va_ok = true;
      bool vb_ok = true;

      val_actions.add_action(
          [](model::RunnableActionSet &set, auto *self, bool &ok,
             NondeterministicProposalData &proposal,
             SignedTransaction &tx) -> model::Async {
            co_await set.bg();
            auto handler = self->make_new_system_call_handler_validation(
                tx.tx, *self->context_, proposal);
            auto res = handler.root_invoke();
            if (!res.is_root_success()) {
              ok = false;
              co_return;
            }
            auto commit_res = handler.try_commit_transaction();
            if (commit_res != ExecResult::ExecResultOK) {
              ok = false;
            }
            co_return;
          },
          this, va_ok, a_proposal, insert_tx_a);

      val_actions.add_action(
          [](model::RunnableActionSet &set, auto *self, bool &ok,
             NondeterministicProposalData &proposal,
             SignedTransaction &tx) -> model::Async {
            co_await set.bg();
            auto handler = self->make_new_system_call_handler_validation(
                tx.tx, *self->context_, proposal);
            auto res = handler.root_invoke();
            if (!res.is_root_success()) {
              ok = false;
              co_return;
            }
            auto commit_res = handler.try_commit_transaction();
            if (commit_res != ExecResult::ExecResultOK) {
              ok = false;
            }
            co_return;
          },
          this, vb_ok, b_proposal, insert_tx_b);

      ASSERT_EQ(val_actions.run(), model::ActionResult::kOk);
      EXPECT_EQ(va_ok, a_ok) << "Commit/abort agreement for TX_A";
      EXPECT_EQ(vb_ok, b_ok) << "Commit/abort agreement for TX_B";

      // Discard validation, reopen from same base state for next iteration
      discard_block(BlockExecutionMode::kBlockExecutionModeValidation);
      validation_wq.advance_cursor();
    }

    work_queue.advance_cursor();
  }
}

// Scenario C: Inserter + Concurrent Consumer (CRITICAL).
// TX_A inserts, TX_B consumes concurrent entry from A.
// Exercises the consumed_but_not_inserted mechanism when validation
// commits B before A.
TEST_F(RCQInterleavingTest, InserterAndConcurrentConsumer)
{
  auto insert_tx = make_insert_tx({10});
  auto consume_tx = make_consume_tx(1);

  model::WorkQueue work_queue;

  while (!work_queue.done()) {
    advance_block();

    model::RunnableActionSet actions(work_queue);

    bool a_ok = true;
    bool b_ok = true;
    NondeterministicProposalData a_proposal;
    NondeterministicProposalData b_proposal;

    // TX_A: insert {10}
    actions.add_action(
        [](model::RunnableActionSet &set, auto *self, bool &ok,
           NondeterministicProposalData &proposal,
           SignedTransaction &tx) -> model::Async {
          co_await set.bg();
          auto handler =
              self->make_new_system_call_handler(tx, *self->context_);
          auto res = handler.root_invoke();
          if (!res.is_root_success()) {
            ok = false;
            co_return;
          }
          proposal = handler.get_proposal_data();
          auto commit_res = handler.try_commit_transaction();
          if (commit_res != ExecResult::ExecResultOK &&
              commit_res != ExecResult::ExecResultTxNonDeterministicError) {
            throw std::runtime_error("try_commit_transaction error");
          }
          if (commit_res == ExecResult::ExecResultTxNonDeterministicError) {
            ok = false;
          }
          co_return;
        },
        this, a_ok, a_proposal, insert_tx);

    // TX_B: consume 1 entry (concurrent or empty)
    actions.add_action(
        [](model::RunnableActionSet &set, auto *self, bool &ok,
           NondeterministicProposalData &proposal,
           SignedTransaction &tx) -> model::Async {
          co_await set.bg();
          auto handler =
              self->make_new_system_call_handler(tx, *self->context_);
          auto res = handler.root_invoke();
          if (!res.is_root_success()) {
            ok = false;
            co_return;
          }
          proposal = handler.get_proposal_data();
          auto commit_res = handler.try_commit_transaction();
          if (commit_res != ExecResult::ExecResultOK &&
              commit_res != ExecResult::ExecResultTxNonDeterministicError) {
            throw std::runtime_error("try_commit_transaction error");
          }
          if (commit_res == ExecResult::ExecResultTxNonDeterministicError) {
            ok = false;
          }
          co_return;
        },
        this, b_ok, b_proposal, consume_tx);

    ASSERT_EQ(actions.run(), model::ActionResult::kOk);

    // Record proposal results, discard proposal block (don't commit to DB)
    bool proposal_a = a_ok;
    bool proposal_b = b_ok;
    discard_block(BlockExecutionMode::kBlockExecutionModeValidation);

    // Validate: enumerate all interleavings from same base state
    model::WorkQueue validation_wq;
    while (!validation_wq.done()) {
      model::RunnableActionSet val_actions(validation_wq);
      bool va_ok = true;
      bool vb_ok = true;

      if (proposal_a) {
        val_actions.add_action(
            [](model::RunnableActionSet &set, auto *self, bool &ok,
               NondeterministicProposalData &proposal,
               SignedTransaction &tx) -> model::Async {
              co_await set.bg();
              auto handler = self->make_new_system_call_handler_validation(
                  tx.tx, *self->context_, proposal);
              auto res = handler.root_invoke();
              if (!res.is_root_success()) {
                ok = false;
                co_return;
              }
              auto commit_res = handler.try_commit_transaction();
              if (commit_res != ExecResult::ExecResultOK &&
                  commit_res !=
                      ExecResult::ExecResultTxNonDeterministicError) {
                throw std::runtime_error("try_commit_transaction error");
              }
              if (commit_res ==
                  ExecResult::ExecResultTxNonDeterministicError) {
                ok = false;
              }
              co_return;
            },
            this, va_ok, a_proposal, insert_tx);
      }

      if (proposal_b) {
        val_actions.add_action(
            [](model::RunnableActionSet &set, auto *self, bool &ok,
               NondeterministicProposalData &proposal,
               SignedTransaction &tx) -> model::Async {
              co_await set.bg();
              auto handler = self->make_new_system_call_handler_validation(
                  tx.tx, *self->context_, proposal);
              auto res = handler.root_invoke();
              if (!res.is_root_success()) {
                ok = false;
                co_return;
              }
              auto commit_res = handler.try_commit_transaction();
              if (commit_res != ExecResult::ExecResultOK &&
                  commit_res !=
                      ExecResult::ExecResultTxNonDeterministicError) {
                throw std::runtime_error("try_commit_transaction error");
              }
              if (commit_res ==
                  ExecResult::ExecResultTxNonDeterministicError) {
                ok = false;
              }
              co_return;
            },
            this, vb_ok, b_proposal, consume_tx);
      }

      ASSERT_EQ(val_actions.run(), model::ActionResult::kOk);
      EXPECT_EQ(va_ok, proposal_a) << "Commit/abort agreement for insert TX";
      EXPECT_EQ(vb_ok, proposal_b) << "Commit/abort agreement for consume TX";

      // Discard validation, reopen from same base state
      discard_block(BlockExecutionMode::kBlockExecutionModeValidation);
      validation_wq.advance_cursor();
    }

    work_queue.advance_cursor();
  }
}

// Scenario D: Two Inserters + One Consumer.
// TX_A inserts {10}, TX_B inserts {20}, TX_C consumes concurrent entries.
// Tests multi-source concurrent entry reconciliation.
TEST_F(RCQInterleavingTest, TwoInsertersOneConsumer)
{
  auto insert_tx_a = make_insert_tx({10});
  auto insert_tx_b = make_insert_tx({20});
  auto consume_tx = make_consume_tx(2);

  model::WorkQueue work_queue;

  while (!work_queue.done()) {
    advance_block();

    model::RunnableActionSet actions(work_queue);

    bool a_ok = true, b_ok = true, c_ok = true;
    NondeterministicProposalData a_proposal, b_proposal, c_proposal;

    auto add_proposal_action = [this](model::RunnableActionSet &actions,
                                      bool &ok,
                                      NondeterministicProposalData &proposal,
                                      SignedTransaction &tx) {
      actions.add_action(
          [](model::RunnableActionSet &set, auto *self, bool &ok,
             NondeterministicProposalData &proposal,
             SignedTransaction &tx) -> model::Async {
            co_await set.bg();
            auto handler =
                self->make_new_system_call_handler(tx, *self->context_);
            auto res = handler.root_invoke();
            if (!res.is_root_success()) {
              ok = false;
              co_return;
            }
            proposal = handler.get_proposal_data();
            auto commit_res = handler.try_commit_transaction();
            if (commit_res != ExecResult::ExecResultOK &&
                commit_res !=
                    ExecResult::ExecResultTxNonDeterministicError) {
              throw std::runtime_error("try_commit_transaction error");
            }
            if (commit_res == ExecResult::ExecResultTxNonDeterministicError) {
              ok = false;
            }
            co_return;
          },
          this, ok, proposal, tx);
    };

    add_proposal_action(actions, a_ok, a_proposal, insert_tx_a);
    add_proposal_action(actions, b_ok, b_proposal, insert_tx_b);
    add_proposal_action(actions, c_ok, c_proposal, consume_tx);

    ASSERT_EQ(actions.run(), model::ActionResult::kOk);

    bool pa = a_ok, pb = b_ok, pc = c_ok;
    discard_block(BlockExecutionMode::kBlockExecutionModeValidation);

    // Validate all interleavings from same base state
    model::WorkQueue validation_wq;
    while (!validation_wq.done()) {
      model::RunnableActionSet val_actions(validation_wq);
      bool va = true, vb = true, vc = true;

      auto add_validation_action =
          [this](model::RunnableActionSet &actions, bool &ok,
                 NondeterministicProposalData &proposal,
                 SignedTransaction &tx) {
            actions.add_action(
                [](model::RunnableActionSet &set, auto *self, bool &ok,
                   NondeterministicProposalData &proposal,
                   SignedTransaction &tx) -> model::Async {
                  co_await set.bg();
                  auto handler =
                      self->make_new_system_call_handler_validation(
                          tx.tx, *self->context_, proposal);
                  auto res = handler.root_invoke();
                  if (!res.is_root_success()) {
                    ok = false;
                    co_return;
                  }
                  auto commit_res = handler.try_commit_transaction();
                  if (commit_res != ExecResult::ExecResultOK &&
                      commit_res !=
                          ExecResult::ExecResultTxNonDeterministicError) {
                    throw std::runtime_error("try_commit_transaction error");
                  }
                  if (commit_res ==
                      ExecResult::ExecResultTxNonDeterministicError) {
                    ok = false;
                  }
                  co_return;
                },
                this, ok, proposal, tx);
          };

      if (pa) {
        add_validation_action(val_actions, va, a_proposal, insert_tx_a);
      }
      if (pb) {
        add_validation_action(val_actions, vb, b_proposal, insert_tx_b);
      }
      if (pc) {
        add_validation_action(val_actions, vc, c_proposal, consume_tx);
      }

      ASSERT_EQ(val_actions.run(), model::ActionResult::kOk);
      EXPECT_EQ(va, pa) << "Commit/abort agreement for TX_A";
      EXPECT_EQ(vb, pb) << "Commit/abort agreement for TX_B";
      EXPECT_EQ(vc, pc) << "Commit/abort agreement for TX_C";

      discard_block(BlockExecutionMode::kBlockExecutionModeValidation);
      validation_wq.advance_cursor();
    }

    work_queue.advance_cursor();
  }
}

// Scenario G: Duplicate Entry Values.
// TX_A inserts {10}, TX_B inserts {10} (same value),
// TX_C consumes 2 concurrent entries.
// Targets the L13 audit finding about consumed_but_not_inserted multiset.
TEST_F(RCQInterleavingTest, DuplicateEntryValues)
{
  auto insert_tx_a = make_insert_tx({10});
  auto insert_tx_b = make_insert_tx({10}); // same value
  auto consume_tx = make_consume_tx(2);

  model::WorkQueue work_queue;

  while (!work_queue.done()) {
    advance_block();

    model::RunnableActionSet actions(work_queue);

    bool a_ok = true, b_ok = true, c_ok = true;
    NondeterministicProposalData a_proposal, b_proposal, c_proposal;

    auto add_proposal_action = [this](model::RunnableActionSet &actions,
                                      bool &ok,
                                      NondeterministicProposalData &proposal,
                                      SignedTransaction &tx) {
      actions.add_action(
          [](model::RunnableActionSet &set, auto *self, bool &ok,
             NondeterministicProposalData &proposal,
             SignedTransaction &tx) -> model::Async {
            co_await set.bg();
            auto handler =
                self->make_new_system_call_handler(tx, *self->context_);
            auto res = handler.root_invoke();
            if (!res.is_root_success()) {
              ok = false;
              co_return;
            }
            proposal = handler.get_proposal_data();
            auto commit_res = handler.try_commit_transaction();
            if (commit_res != ExecResult::ExecResultOK &&
                commit_res !=
                    ExecResult::ExecResultTxNonDeterministicError) {
              throw std::runtime_error("try_commit_transaction error");
            }
            if (commit_res == ExecResult::ExecResultTxNonDeterministicError) {
              ok = false;
            }
            co_return;
          },
          this, ok, proposal, tx);
    };

    add_proposal_action(actions, a_ok, a_proposal, insert_tx_a);
    add_proposal_action(actions, b_ok, b_proposal, insert_tx_b);
    add_proposal_action(actions, c_ok, c_proposal, consume_tx);

    ASSERT_EQ(actions.run(), model::ActionResult::kOk);

    bool pa = a_ok, pb = b_ok, pc = c_ok;
    discard_block(BlockExecutionMode::kBlockExecutionModeValidation);

    model::WorkQueue validation_wq;
    while (!validation_wq.done()) {
      model::RunnableActionSet val_actions(validation_wq);
      bool va = true, vb = true, vc = true;

      auto add_validation_action =
          [this](model::RunnableActionSet &actions, bool &ok,
                 NondeterministicProposalData &proposal,
                 SignedTransaction &tx) {
            actions.add_action(
                [](model::RunnableActionSet &set, auto *self, bool &ok,
                   NondeterministicProposalData &proposal,
                   SignedTransaction &tx) -> model::Async {
                  co_await set.bg();
                  auto handler =
                      self->make_new_system_call_handler_validation(
                          tx.tx, *self->context_, proposal);
                  auto res = handler.root_invoke();
                  if (!res.is_root_success()) {
                    ok = false;
                    co_return;
                  }
                  auto commit_res = handler.try_commit_transaction();
                  if (commit_res != ExecResult::ExecResultOK &&
                      commit_res !=
                          ExecResult::ExecResultTxNonDeterministicError) {
                    throw std::runtime_error("try_commit_transaction error");
                  }
                  if (commit_res ==
                      ExecResult::ExecResultTxNonDeterministicError) {
                    ok = false;
                  }
                  co_return;
                },
                this, ok, proposal, tx);
          };

      if (pa) {
        add_validation_action(val_actions, va, a_proposal, insert_tx_a);
      }
      if (pb) {
        add_validation_action(val_actions, vb, b_proposal, insert_tx_b);
      }
      if (pc) {
        add_validation_action(val_actions, vc, c_proposal, consume_tx);
      }

      ASSERT_EQ(val_actions.run(), model::ActionResult::kOk);
      EXPECT_EQ(va, pa) << "Commit/abort agreement for TX_A";
      EXPECT_EQ(vb, pb) << "Commit/abort agreement for TX_B";
      EXPECT_EQ(vc, pc) << "Commit/abort agreement for TX_C";

      discard_block(BlockExecutionMode::kBlockExecutionModeValidation);
      validation_wq.advance_cursor();
    }

    work_queue.advance_cursor();
  }
}

// NOLINTEND(bugprone-unchecked-optional-access)

} // namespace hogd
