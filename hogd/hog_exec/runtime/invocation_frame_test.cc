#include <gtest/gtest.h>

#include <format>
#include <memory>
#include <span>
#include <string>
#include <utility>

#include "auth_token.hh"
#include "ffi_enums.hh"
#include "hog_exec_constants.hh"
#include "hogd/base/network_config_params.h"
#include "hogd/base/types.h"
#include "hogd/hog_exec/common/status.h"
#include "hogd/hog_exec/components/execution_mode.h"
#include "hogd/hog_exec/runtime/invocation_frame.h"
#include "hogd/hog_exec/runtime/item/database_proxy.h"
#include "hogd/hog_exec/runtime/test_util/block_snapshot_server.h"
#include "invoke_flag.hh"
#include "rpc_types.hh"
#include "transaction.hh"
#include "types.hh"

namespace hogd {

class InvocationFrameTest : public ::testing::Test {
protected:
  void SetUp() override
  {
    server_.start_server(/*port=*/0);
    db_proxy_ = std::make_unique<DatabaseProxy>(
        LocalMockDB{.read_port_number_ = server_.port()});
    per_tx_db_proxy_ =
        std::make_unique<PerTransactionDatabaseProxy>(*db_proxy_, kBlockId,
                                                      kTxId);
  }

  static constexpr BlockID kBlockId = 1;
  static constexpr TransactionID kTxId = 0;
  // The chain's defaults; these tests only need parameters that admit the
  // frames they start.
  static constexpr NetworkConfigParams kConfigParams{};

  BlockSnapshotServer server_ = BlockSnapshotServer(kBlockId);
  std::unique_ptr<DatabaseProxy> db_proxy_;
  std::unique_ptr<PerTransactionDatabaseProxy> per_tx_db_proxy_;
  ExecutionModeData execution_mode_data_ = ProposalModeData{};

  constexpr static ContractAddress make_test_address(uint8_t id)
  {
    ContractAddress address{};
    address[0] = id;
    return address;
  }
};

TEST_F(InvocationFrameTest, MintAuthToken)
{
  InvocationFrameStack stack(nullptr, *per_tx_db_proxy_, kConfigParams);
  ASSERT_TRUE(
      stack.start_frame(make_test_address(0), Calldata{}, 0, {}).has_value());
  AuthToken auth_token{.mint_address = make_test_address(0),
                       .expiry_height = 100,
                       .root_node = {.consumer_address = make_test_address(1),
                                     .payload = {},
                                     .child_nodes = {}}};

  auto token_id = stack.mint_auth_token(auth_token);
  ASSERT_TRUE(token_id.has_value());
  ASSERT_EQ(token_id.value(), 0);

  ASSERT_TRUE(
      stack.check_auth_tokens_owned(std::span<uint32_t>{&token_id.value(), 1}));

  ASSERT_TRUE(
      stack.start_frame(make_test_address(1), Calldata{}, 0, {}).has_value());
  ASSERT_FALSE(
      stack.check_auth_tokens_owned(std::span<uint32_t>{&token_id.value(), 1}));
}

TEST_F(InvocationFrameTest, ForwardAuthToken)
{
  InvocationFrameStack stack(nullptr, *per_tx_db_proxy_, kConfigParams);
  ASSERT_TRUE(
      stack.start_frame(make_test_address(0), Calldata{}, 0, {}).has_value());
  AuthToken auth_token{.mint_address = make_test_address(0),
                       .expiry_height = 100,
                       .root_node = {.consumer_address = make_test_address(1),
                                     .payload = {},
                                     .child_nodes = {}}};

  auto token_id = stack.mint_auth_token(auth_token);
  ASSERT_TRUE(token_id.has_value());
  ASSERT_EQ(token_id.value(), 0);

  ASSERT_TRUE(
      stack.check_auth_tokens_owned(std::span<uint32_t>{&token_id.value(), 1}));

  ASSERT_TRUE(stack
                  .start_frame(make_test_address(1), Calldata{}, 0,
                               std::span<uint32_t>{&token_id.value(), 1})
                  .has_value());
  ASSERT_TRUE(
      stack.check_auth_tokens_owned(std::span<uint32_t>{&token_id.value(), 1}));

  // frame commits successfully
  auto result =
      stack.finish_frame(InvokeStatus<void>{}, execution_mode_data_)();
  ASSERT_TRUE(result.has_value());
  ASSERT_TRUE(result.value().first.result_.has_value());
  ASSERT_EQ(result.value().second, false);

  // now the root frame no longer owns the token
  ASSERT_FALSE(
      stack.check_auth_tokens_owned(std::span<uint32_t>{&token_id.value(), 1}));
  ASSERT_EQ(stack.lookup_auth_token(token_id.value()), nullptr);
}

TEST_F(InvocationFrameTest, RevertsRestoreToken)
{
  InvocationFrameStack stack(nullptr, *per_tx_db_proxy_, kConfigParams);
  ASSERT_TRUE(
      stack.start_frame(make_test_address(0), Calldata{}, 0, {}).has_value());
  AuthToken auth_token{.mint_address = make_test_address(0),
                       .expiry_height = 100,
                       .root_node = {.consumer_address = make_test_address(1),
                                     .payload = {},
                                     .child_nodes = {}}};

  auto token_id = stack.mint_auth_token(auth_token);
  ASSERT_TRUE(token_id.has_value());
  ASSERT_EQ(token_id.value(), 0);

  ASSERT_TRUE(
      stack.check_auth_tokens_owned(std::span<uint32_t>{&token_id.value(), 1}));

  ASSERT_TRUE(stack
                  .start_frame(make_test_address(1), Calldata{}, 0,
                               std::span<uint32_t>{&token_id.value(), 1})
                  .has_value());
  ASSERT_TRUE(
      stack.check_auth_tokens_owned(std::span<uint32_t>{&token_id.value(), 1}));

  // child frame mints a token
  AuthToken child_auth_token{.mint_address = make_test_address(1),
                             .expiry_height = 100,
                             .root_node = {.consumer_address =
                                               make_test_address(2),
                                           .payload = {},
                                           .child_nodes = {}}};
  auto child_token_id = stack.mint_auth_token(child_auth_token);
  ASSERT_TRUE(child_token_id.has_value());
  ASSERT_EQ(child_token_id.value(), 1);

  ASSERT_TRUE(stack.check_auth_tokens_owned(
      std::span<uint32_t>{&child_token_id.value(), 1}));

  // frame reverts and restores the token
  stack.set_active_frame_return(
      InvocationFrameOutput(InvocationFrameStatusCode::Fault));
  auto result = stack.finish_frame(InvokeError{InvokeErrorCode::Deterministic},
                                   execution_mode_data_)();
  ASSERT_TRUE(result.has_value());
  ASSERT_FALSE(result.value().first.result_.has_value());
  ASSERT_EQ(result.value().second, true);

  // now the root frame owns the token
  ASSERT_TRUE(
      stack.check_auth_tokens_owned(std::span<uint32_t>{&token_id.value(), 1}));
  ASSERT_NE(stack.lookup_auth_token(token_id.value()), nullptr);

  // child frame token is not owned
  ASSERT_FALSE(stack.check_auth_tokens_owned(
      std::span<uint32_t>{&child_token_id.value(), 1}));
  ASSERT_EQ(stack.lookup_auth_token(child_token_id.value()), nullptr);
}

TEST_F(InvocationFrameTest, ManualRevertReclaims)
{
  InvocationFrameStack stack(nullptr, *per_tx_db_proxy_, kConfigParams);
  ASSERT_TRUE(
      stack.start_frame(make_test_address(0), Calldata{}, 0, {}).has_value());

  AuthToken auth_token{.mint_address = make_test_address(0),
                       .expiry_height = 100,
                       .root_node = {.consumer_address = make_test_address(1),
                                     .payload = {},
                                     .child_nodes = {}}};

  auto token_id = stack.mint_auth_token(auth_token);
  ASSERT_TRUE(token_id.has_value());

  ASSERT_TRUE(
      stack.check_auth_tokens_owned(std::span<uint32_t>{&token_id.value(), 1}));

  // forward token to child
  ASSERT_TRUE(stack
                  .start_frame(make_test_address(1), Calldata{}, 0,
                               std::span<uint32_t>{&token_id.value(), 1})
                  .has_value());
  ASSERT_TRUE(
      stack.check_auth_tokens_owned(std::span<uint32_t>{&token_id.value(), 1}));

  // child manually reverts: invoke succeeds but frame output is Fault
  stack.set_active_frame_return(
      InvocationFrameOutput(InvocationFrameStatusCode::Fault));
  auto result =
      stack.finish_frame(InvokeStatus<void>{}, execution_mode_data_)();
  ASSERT_TRUE(result.has_value());
  ASSERT_TRUE(result.value().first.result_.has_value());
  ASSERT_EQ(result.value().second, true);

  // parent must own the token again
  ASSERT_TRUE(
      stack.check_auth_tokens_owned(std::span<uint32_t>{&token_id.value(), 1}));
  ASSERT_NE(stack.lookup_auth_token(token_id.value()), nullptr);
}

// The child TryInvoke depth cap is a deterministic rule of the frame stack, so
// it must hold identically in every execution mode. Each mode takes its own
// arm in finish_frame to decide whether a child succeeded, and a bump missed
// in one arm lets that mode run past the cap (viewing did, #3943 audit).
class InvocationFrameDepthCapTest
  : public InvocationFrameTest,
    public ::testing::WithParamInterface<ExecutionMode> {
protected:
  // One slot is reserved for root commit, so the child cap is
  // kMaxSavepointDepth - 1.
  static constexpr uint32_t kChildCap = kMaxSavepointDepth - 1;

  static NondeterministicProposalData all_true_bits(uint32_t count)
  {
    NondeterministicProposalData ndet{};
    for (uint32_t i = 0; i < (count + 7) / 8; i++) {
      ndet.nondeterministic_bits.push_back(0xff);
    }
    ndet.nondeterministic_bit_count = count;
    return ndet;
  }

  // Mode data under which every child TryInvoke that reaches the mode's arm
  // succeeds: proposal probes locks nothing contends for, validation replays
  // a true bit per child, and viewing is left to its default of true. Only
  // the depth cap can then fail a child.
  static ExecutionModeData make_mode_data(ExecutionMode mode)
  {
    if (mode == ExecutionMode::kValidating) {
      return ExecutionModeData{std::in_place_type<ValidationModeData>,
                               all_true_bits(kChildCap + 1)};
    }
    if (mode == ExecutionMode::kViewing) {
      ViewingModeCtor ctor{.proposal_data_segment_ = {}};
      return ExecutionModeData{std::in_place_type<ViewingModeData>, ctor};
    }
    return ProposalModeData{};
  }
};

TEST_P(InvocationFrameDepthCapTest, TryInvokeHitsSavepointDepthLimit)
{
  ExecutionModeData mode_data = make_mode_data(GetParam());

  InvocationFrameStack stack(nullptr, *per_tx_db_proxy_, kConfigParams);
  ASSERT_TRUE(
      stack.start_frame(make_test_address(0), Calldata{}, 0, {}).has_value());

  for (uint32_t i = 0; i < kChildCap; i++) {
    ASSERT_TRUE(stack
                    .start_frame(make_test_address(1), Calldata{},
                                 std::to_underlying(InvokeFlag::TryInvoke), {})
                    .has_value());
    auto result = stack.finish_frame(InvokeStatus<void>{}, mode_data)();
    ASSERT_TRUE(result.has_value()) << "iteration " << i;
    ASSERT_TRUE(result.value().first.result_.has_value()) << "iteration " << i;
    ASSERT_EQ(result.value().second, false);
  }

  ASSERT_TRUE(stack
                  .start_frame(make_test_address(1), Calldata{},
                               std::to_underlying(InvokeFlag::TryInvoke), {})
                  .has_value());
  auto result = stack.finish_frame(InvokeStatus<void>{}, mode_data)();
  ASSERT_TRUE(result.has_value());
  ASSERT_FALSE(result.value().first.result_.has_value())
      << "child TryInvoke past the cap must fail deterministically in "
      << std::format("{}", GetParam());
}

INSTANTIATE_TEST_SUITE_P(
    ExecutionModes, InvocationFrameDepthCapTest,
    ::testing::Values(ExecutionMode::kProposing, ExecutionMode::kValidating,
                      ExecutionMode::kViewing),
    [](::testing::TestParamInfo<ExecutionMode> const &info) -> std::string {
      switch (info.param) {
      case ExecutionMode::kProposing:
        return "Proposing";
      case ExecutionMode::kValidating:
        return "Validating";
      case ExecutionMode::kViewing:
        return "Viewing";
      }
      return "Unknown";
    });

TEST_F(InvocationFrameTest, StartFrameReportsSuccess)
{
  // Frame admission is status-returning so that an allocation failure surfaces
  // as an Unrecoverable SystemError instead of terminating the executor across
  // SystemCallHandler::start_frame()'s noexcept boundary (#3058). On the normal
  // path it reports success, including when a token is forwarded into the
  // child.
  InvocationFrameStack stack(nullptr, *per_tx_db_proxy_, kConfigParams);
  ASSERT_TRUE(
      stack.start_frame(make_test_address(0), Calldata{}, 0, {}).has_value());

  AuthToken auth_token{.mint_address = make_test_address(0),
                       .expiry_height = 100,
                       .root_node = {.consumer_address = make_test_address(1),
                                     .payload = {},
                                     .child_nodes = {}}};
  auto token_id = stack.mint_auth_token(auth_token);
  ASSERT_TRUE(token_id.has_value());

  ASSERT_TRUE(stack
                  .start_frame(make_test_address(1), Calldata{}, 0,
                               std::span<uint32_t>{&token_id.value(), 1})
                  .has_value());
  ASSERT_TRUE(
      stack.check_auth_tokens_owned(std::span<uint32_t>{&token_id.value(), 1}));
}

TEST_F(InvocationFrameTest, ManualRevertReclaimsThenReforward)
{
  InvocationFrameStack stack(nullptr, *per_tx_db_proxy_, kConfigParams);
  ASSERT_TRUE(
      stack.start_frame(make_test_address(0), Calldata{}, 0, {}).has_value());

  AuthToken auth_token{.mint_address = make_test_address(0),
                       .expiry_height = 100,
                       .root_node = {.consumer_address = make_test_address(1),
                                     .payload = {},
                                     .child_nodes = {}}};

  auto token_id = stack.mint_auth_token(auth_token);
  ASSERT_TRUE(token_id.has_value());

  // forward token to child, which manually reverts
  ASSERT_TRUE(stack
                  .start_frame(make_test_address(1), Calldata{}, 0,
                               std::span<uint32_t>{&token_id.value(), 1})
                  .has_value());
  stack.set_active_frame_return(
      InvocationFrameOutput(InvocationFrameStatusCode::Fault));
  auto result1 =
      stack.finish_frame(InvokeStatus<void>{}, execution_mode_data_)();
  ASSERT_TRUE(result1.has_value());
  ASSERT_EQ(result1.value().second, true);

  // token reclaimed by parent
  ASSERT_TRUE(
      stack.check_auth_tokens_owned(std::span<uint32_t>{&token_id.value(), 1}));

  // re-forward the same token to a new child that succeeds
  ASSERT_TRUE(stack
                  .start_frame(make_test_address(2), Calldata{}, 0,
                               std::span<uint32_t>{&token_id.value(), 1})
                  .has_value());
  ASSERT_TRUE(
      stack.check_auth_tokens_owned(std::span<uint32_t>{&token_id.value(), 1}));

  auto result2 =
      stack.finish_frame(InvokeStatus<void>{}, execution_mode_data_)();
  ASSERT_TRUE(result2.has_value());
  ASSERT_TRUE(result2.value().first.result_.has_value());
  ASSERT_EQ(result2.value().second, false);

  // parent no longer owns the token (successful child keeps it)
  ASSERT_FALSE(
      stack.check_auth_tokens_owned(std::span<uint32_t>{&token_id.value(), 1}));
}

} // namespace hogd
