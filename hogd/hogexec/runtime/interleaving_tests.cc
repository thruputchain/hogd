#include <gtest/gtest.h>

#include <cassert>
#include <cstdint>
#include <cstring>

#include <memory>
#include <stdexcept>
#include <tuple>
#include <unordered_map>
#include <utility>

#include "bcs/encode.h"
#include "ffi_enums.hh"
#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h"
#include "hogd/crypto/crypto.h"
#include "hogd/db_interface/block_manager.h"
#include "hogd/hogexec/runtime/contract_trait.h"
#include "hogd/hogexec/runtime/native_contract_test_framework.h"
#include "hogd/hogexec/runtime/native_runtime.h"
#include "hogd/hogexec/runtime/status.h"
#include "hogd/hogexec/runtime/syscall_handler.h"
#include "hogd/mockdb/in_memory_database.h"
#include "model_checker/async.h"
#include "model_checker/work_queue.h"
#include "rpc_types.hh"
#include "storage_item.hh"
#include "transaction.hh"
#include "types.hh"

namespace hogd {

class InterleavingTest : public ::testing::Test,
                         public NativeContractTestFramework {
public:
  InterleavingTest() : NativeContractTestFramework(false) {}

protected:
  void SetUp() override {}
};

// NOLINTBEGIN(bugprone-unchecked-optional-access)
// Check that for any interleaving that passes proposal, it also passes
// validation with the same results.
// This test is bizarrely slow on clang in CI.
TEST_F(InterleavingTest, DISABLED_NNIntAddInterleavings)
{
  NativeContractBuilder builder;

  StorageKeyContractData key = make_storage_key("test");

  // perform a set_add_nnint
  builder.add(0,
              [&](SystemCallHandler &handler, int128_t set_amount,
                  int128_t add_amount) -> InvokeStatus<void> {
                auto r = handler.set_nnint(key, set_amount, add_amount);
                if (!r.has_value() || !r.value().has_value()) {
                  return InvokeError{InvokeErrorCode::kInvokeDeterministic};
                }
                return {};
              });
  // reset contract state
  builder.add(1, [&](SystemCallHandler &handler) -> InvokeStatus<void> {
    auto r = handler.set_nnint(key, 35, 0);
    if (!r.has_value() || !r.value().has_value()) {
      return InvokeError{InvokeErrorCode::kInvokeDeterministic};
    }
    return {};
  });

  auto native_contract = builder.build();

  std::unordered_map<uint32_t, std::unique_ptr<NativeContract>> contract_map;
  constexpr uint32_t kTestContractId = 0;
  contract_map.emplace(kTestContractId, std::move(native_contract));
  auto runtime_context =
      std::make_shared<NativeContext>(std::move(contract_map));

  // Deploy contracts
  auto test_contract_xdr = make_native_contract(kTestContractId);
  in_memory_db_.write_contract(sha256(*test_contract_xdr), test_contract_xdr);
  in_memory_db_.write_deployment(ContractAddress{}, sha256(*test_contract_xdr));

  // Grant initial tokens to test contract
  setup_grant_storage_token(ContractAddress{}, 1'000'000'000);

  StorageKey wrapped_key(StorageKeyType::kStorageKeyTypeContractData);
  wrapped_key.contract_data_key() = key;

  model::WorkQueue work_queue;

  auto reset_state = [&]() {
    auto handler = make_new_system_call_handler(
        SignedTransaction{
            .tx =
                Transaction{
                    .invocation =
                        Invocation{.invoked_address = ContractAddress{},
                                   .entrypoint = entrypoint_from_method_id(1),
                                   .gas_limit = UINT64_MAX,
                                   .calldata = {}}}},
        runtime_context);
    auto res = handler.root_invoke();
    if (!res.is_root_success()) {
      throw std::runtime_error("root invoke for reset transaction failed");
    }
    auto commit_res = handler.try_commit_transaction();
    if (!commit_res.has_value() || !commit_res.value()) {
      throw std::runtime_error(
          "try_commit_transaction error for reset transaction");
    }
  };

  while (!work_queue.done()) {
    reset_state();
    advance_block();

    model::RunnableActionSet actions(work_queue);

    auto add_set_add_action =
        [this, &runtime_context]<int128_t SetAmount, int128_t AddAmount>(
            model::RunnableActionSet &actions, bool &action_result) {
          actions.add_action(
              [](model::RunnableActionSet &set, auto self,
                 auto &runtime_context, bool &action_result) -> model::Async {
                // set_amount
                co_await set.bg();

                auto calldata = bcs::encode_to<Calldata>(
                    std::tuple<int128_t, int128_t>{SetAmount, AddAmount});
                assert(calldata.has_value());

                auto handler = self->make_new_system_call_handler(
                    SignedTransaction{
                        .tx =
                            Transaction{
                                .invocation =
                                    Invocation{.invoked_address =
                                                   ContractAddress{},
                                               .entrypoint =
                                                   entrypoint_from_method_id(0),
                                               .gas_limit = UINT64_MAX,
                                               .calldata = calldata.value()}}},
                    runtime_context);
                auto res = handler.root_invoke();
                if (!res.is_root_success()) {
                  throw std::runtime_error("root invoke failed");
                }
                auto commit_res = handler.try_commit_transaction();
                if (!commit_res.has_value()) {
                  throw std::runtime_error("try_commit_transaction error");
                }
                if (!commit_res.value()) {
                  action_result = false;
                  co_return;
                }
                co_return;
              },
              this, runtime_context, action_result);
        };

    bool action_1 = true;
    add_set_add_action.template operator()<100, 10>(actions, action_1);
    bool action_2 = true;
    add_set_add_action.template operator()<100, -70>(actions, action_2);
    bool action_3 = true;
    add_set_add_action.template operator()<100, -90>(actions, action_3);
    ASSERT_EQ(actions.run(), model::ActionResult::kOk);

    EXPECT_TRUE(action_1);
    EXPECT_FALSE(action_1 && action_2 && action_3);

    advance_block();

    auto res = lookup_storage_item_from_db(
        AddressAndKey{.address = ContractAddress{}, .key = wrapped_key});
    ASSERT_TRUE(res.has_value());
    ASSERT_EQ(res.value().type(), StorageItemType::kItemTypeNonnegativeInt);
    int128_t proposal_value = from_xdr(res.value().nnint());

    model::WorkQueue work_queue_validation;
    while (!work_queue_validation.done()) {
      reset_state();
      advance_block(BlockExecutionMode::kBlockExecutionModeValidation);

      model::RunnableActionSet actions_validation(work_queue_validation);

      bool action_1_validation = action_1;
      if (action_1) {
        add_set_add_action.template operator()<100, 10>(actions_validation,
                                                        action_1_validation);
      }
      bool action_2_validation = action_2;
      if (action_2) {
        add_set_add_action.template operator()<100, -70>(actions_validation,
                                                         action_2_validation);
      }
      bool action_3_validation = action_3;
      if (action_3) {
        add_set_add_action.template operator()<100, -90>(actions_validation,
                                                         action_3_validation);
      }

      // for all the actions that are added during the validation round,
      // assert that those actions succeed
      ASSERT_EQ(actions_validation.run(), model::ActionResult::kOk);
      EXPECT_EQ(action_1_validation, action_1);
      EXPECT_EQ(action_2_validation, action_2);
      EXPECT_EQ(action_3_validation, action_3);
      advance_block();

      auto res_validation = lookup_storage_item_from_db(
          AddressAndKey{.address = ContractAddress{}, .key = wrapped_key});
      ASSERT_TRUE(res_validation.has_value());
      ASSERT_EQ(res_validation.value().type(),
                StorageItemType::kItemTypeNonnegativeInt);
      int128_t validation_value = from_xdr(res_validation.value().nnint());

      EXPECT_EQ(proposal_value, validation_value);
      work_queue_validation.advance_cursor();
    }

    work_queue.advance_cursor();
  }
}

// NOLINTEND(bugprone-unchecked-optional-access)

} // namespace hogd
