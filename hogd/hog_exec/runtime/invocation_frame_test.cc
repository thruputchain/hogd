#include <gtest/gtest.h>

#include <algorithm>
#include <format>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "auth_token.hh"
#include "ffi_enums.hh"
#include "hog_exec_constants.hh"
#include "hogd/base/network_config_params.h"
#include "hogd/base/types.h"
#include "hogd/hog_exec/common/status.h"
#include "hogd/hog_exec/components/auth_subtoken.h"
#include "hogd/hog_exec/components/execution_mode.h"
#include "hogd/hog_exec/runtime/invocation_frame.h"
#include "hogd/hog_exec/runtime/item/database_proxy.h"
#include "hogd/hog_exec/runtime/item/item_view.h"
#include "hogd/hog_exec/runtime/test_util/block_snapshot_server.h"
#include "hogd/hog_exec/runtime/validated_auth_tokens.h"
#include "invoke_flag.hh"
#include "rpc_types.hh"
#include "storage_item.hh"
#include "transaction.hh"
#include "types.hh"
#include "x23/serialize.h"

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

  // Mints token, decoded as the view path decodes a forwarded token.
  static SyscallStatus<uint32_t> mint(InvocationFrameStack &stack,
                                      AuthToken const &token)
  {
    RegisteredAuthToken registered;
    x23::deserialize(&registered, x23::serialize(token));
    return stack.mint_auth_token(std::move(registered));
  }

  // A forwarded-token list holding one id, built the way the Invoke syscall
  // builds it.
  static ValidatedAuthTokens forward(uint32_t token_id)
  {
    return *ValidatedAuthTokens::parse(std::as_bytes(std::span(&token_id, 1)));
  }

  static ValidatedAuthTokens forward(std::vector<uint32_t> const &token_ids)
  {
    return *ValidatedAuthTokens::parse(std::as_bytes(std::span(token_ids)));
  }

  // A token minted by address 0 for consumer, with leaf children for the
  // consumers in children.
  static AuthToken make_token(uint8_t consumer,
                              std::vector<uint8_t> const &children = {})
  {
    AuthToken token{.mint_address = make_test_address(0),
                    .expiry_height = 100,
                    .root_node = {.consumer_address =
                                      make_test_address(consumer),
                                  .payload = {consumer},
                                  .child_nodes = {}}};
    for (auto child : children) {
      token.root_node.child_nodes.push_back(
          AuthTokenNode{.consumer_address = make_test_address(child),
                        .payload = {child},
                        .child_nodes = {}});
    }
    return token;
  }

  // The node the active frame holds under token_id, or null if it owns none.
  static AuthSubtoken const *node(InvocationFrameStack const &stack,
                                  uint32_t token_id)
  {
    auto const *token = stack.lookup_auth_token(token_id);
    return token != nullptr ? token->root_node.get() : nullptr;
  }

  void finish_child(InvocationFrameStack &stack, bool expect_revert = false)
  {
    auto result =
        stack.finish_frame(InvokeStatus<void>{}, execution_mode_data_)();
    ASSERT_TRUE(result.has_value());
    ASSERT_EQ(result.value().second, expect_revert);
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

  auto token_id = mint(stack, auth_token);
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

  auto token_id = mint(stack, auth_token);
  ASSERT_TRUE(token_id.has_value());
  ASSERT_EQ(token_id.value(), 0);

  ASSERT_TRUE(
      stack.check_auth_tokens_owned(std::span<uint32_t>{&token_id.value(), 1}));

  ASSERT_TRUE(stack
                  .start_frame(make_test_address(1), Calldata{}, 0,
                               forward(token_id.value()))
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

  auto token_id = mint(stack, auth_token);
  ASSERT_TRUE(token_id.has_value());
  ASSERT_EQ(token_id.value(), 0);

  ASSERT_TRUE(
      stack.check_auth_tokens_owned(std::span<uint32_t>{&token_id.value(), 1}));

  ASSERT_TRUE(stack
                  .start_frame(make_test_address(1), Calldata{}, 0,
                               forward(token_id.value()))
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
  auto child_token_id = mint(stack, child_auth_token);
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

  auto token_id = mint(stack, auth_token);
  ASSERT_TRUE(token_id.has_value());

  ASSERT_TRUE(
      stack.check_auth_tokens_owned(std::span<uint32_t>{&token_id.value(), 1}));

  // forward token to child
  ASSERT_TRUE(stack
                  .start_frame(make_test_address(1), Calldata{}, 0,
                               forward(token_id.value()))
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

// The savepoint cap is a deterministic rule of the frame stack, so it must
// hold identically in every execution mode. Each mode takes its own arm in
// finish_frame to decide whether a child succeeded, and a bump missed in one
// arm lets that mode run past the cap (viewing did, #3943 audit).
class InvocationFrameDepthCapTest
  : public InvocationFrameTest,
    public ::testing::WithParamInterface<ExecutionMode> {
protected:
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
  // the savepoint cap can then fail a child.
  static ExecutionModeData make_mode_data(ExecutionMode mode,
                                          uint32_t try_invokes)
  {
    if (mode == ExecutionMode::kValidating) {
      return ExecutionModeData{std::in_place_type<ValidationModeData>,
                               all_true_bits(try_invokes)};
    }
    if (mode == ExecutionMode::kViewing) {
      ViewingModeCtor ctor{.proposal_data_segment_ = {}};
      return ExecutionModeData{std::in_place_type<ViewingModeData>, ctor};
    }
    return ProposalModeData{};
  }

  static void start(InvocationFrameStack &stack, InvokeFlagSet flags)
  {
    ASSERT_TRUE(stack.start_frame(make_test_address(1), Calldata{}, flags, {})
                    .has_value());
  }

  // Starts and finishes a child TryInvoke of the active frame; returns
  // whether it committed.
  static bool try_invoke(InvocationFrameStack &stack,
                         ExecutionModeData &mode_data)
  {
    EXPECT_TRUE(stack
                    .start_frame(make_test_address(1), Calldata{},
                                 std::to_underlying(InvokeFlag::TryInvoke), {})
                    .has_value());
    auto result = stack.finish_frame(InvokeStatus<void>{}, mode_data)();
    EXPECT_TRUE(result.has_value());
    return result.has_value() && result.value().first.result_.has_value();
  }
};

// Each successful TryInvoke folds its caller's savepoints into one, so
// sequential TryInvokes from one frame never approach the cap, however many
// there are.
TEST_P(InvocationFrameDepthCapTest, SequentialTryInvokesDoNotAccumulate)
{
  static constexpr uint32_t kTryInvokes = kMaxSavepointDepth + 1;
  ExecutionModeData mode_data = make_mode_data(GetParam(), kTryInvokes);

  InvocationFrameStack stack(nullptr, *per_tx_db_proxy_, kConfigParams);
  start(stack, 0);
  for (uint32_t i = 0; i < kTryInvokes; i++) {
    ASSERT_TRUE(try_invoke(stack, mode_data)) << "iteration " << i;
  }
}

// What the cap does bound is savepoints held by open frames at once: one per
// open frame whose call tree committed a TryInvoke. With a configured depth of
// 4, one slot reserved for the root commit, the root and two nested frames can
// hold one each, and a TryInvoke from a third fails.
TEST_P(InvocationFrameDepthCapTest, TryInvokeFailsOnceOpenFramesHoldTheCap)
{
  static constexpr uint32_t kDepth = 4;
  NetworkConfigParams params{};
  params.max_savepoint_depth = kDepth;
  ExecutionModeData mode_data = make_mode_data(GetParam(), kDepth);

  InvocationFrameStack stack(nullptr, *per_tx_db_proxy_, params);
  start(stack, 0);
  for (uint32_t open = 0; open < kDepth - 1; open++) {
    ASSERT_TRUE(try_invoke(stack, mode_data)) << "open frame " << open;
    start(stack, std::to_underlying(InvokeFlag::TryInvoke));
  }
  EXPECT_FALSE(try_invoke(stack, mode_data))
      << "TryInvoke past the cap must fail deterministically in "
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

// The savepoints a TryInvoke push asks hog_db to merge: of each open frame's,
// all but the oldest, with the pushing frame's and the new one counted as its
// parent's.
TEST_F(InvocationFrameTest, TryInvokePushMergesOpenFrameSavepoints)
{
  auto try_invoke = [&](InvocationFrameStack &stack) {
    ASSERT_TRUE(stack
                    .start_frame(make_test_address(1), Calldata{},
                                 std::to_underlying(InvokeFlag::TryInvoke), {})
                    .has_value());
    finish_child(stack);
  };
  auto plain_call_with_try_invoke = [&](InvocationFrameStack &stack) {
    ASSERT_TRUE(
        stack.start_frame(make_test_address(2), Calldata{}, 0, {}).has_value());
    try_invoke(stack);
    finish_child(stack);
  };

  InvocationFrameStack stack(nullptr, *per_tx_db_proxy_, kConfigParams);
  ASSERT_TRUE(
      stack.start_frame(make_test_address(0), Calldata{}, 0, {}).has_value());

  // Savepoint 1; nothing to merge, and the root holds 1 after this.
  try_invoke(stack);
  // Savepoint 2 goes to the plain child, which hands it to the root when it
  // commits: the root holds 1 and 2 after this.
  plain_call_with_try_invoke(stack);
  // Savepoint 3: merges the root's 2 into its 1, and the root holds 1 and 3.
  plain_call_with_try_invoke(stack);
  // Savepoint 4 joins the root's, so 3 and 4 merge into 1.
  try_invoke(stack);

  std::vector<std::pair<uint32_t, std::vector<uint32_t>>> expected{{1, {}},
                                                                   {2, {}},
                                                                   {3, {2}},
                                                                   {4, {3, 4}}};
  EXPECT_EQ(server_.impl()->take_pushed_merges(), expected);
}

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
  auto token_id = mint(stack, auth_token);
  ASSERT_TRUE(token_id.has_value());

  ASSERT_TRUE(stack
                  .start_frame(make_test_address(1), Calldata{}, 0,
                               forward(token_id.value()))
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

  auto token_id = mint(stack, auth_token);
  ASSERT_TRUE(token_id.has_value());

  // forward token to child, which manually reverts
  ASSERT_TRUE(stack
                  .start_frame(make_test_address(1), Calldata{}, 0,
                               forward(token_id.value()))
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
                               forward(token_id.value()))
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

TEST_F(InvocationFrameTest, ChildFramesShareParentBytestring)
{
  // #4303: a sibling frame's first touch must share the root's bytestring.
  AddressAndKey const addrkey{.address = make_test_address(1),
                              .key = StorageKey(StorageKeyType::ContractData)};
  Bytestring const payload(32'768, 0x5a);

  InvocationFrameStack stack(nullptr, *per_tx_db_proxy_, kConfigParams);
  ASSERT_TRUE(
      stack.start_frame(make_test_address(0), Calldata{}, 0, {}).has_value());

  auto finish_child = [&](InvokeStatus<void> status, bool expect_revert) {
    auto result = stack.finish_frame(std::move(status), execution_mode_data_)();
    ASSERT_TRUE(result.has_value());
    ASSERT_EQ(result.value().second, expect_revert);
  };

  ASSERT_TRUE(
      stack.start_frame(make_test_address(1), Calldata{}, 0, {}).has_value());
  ASSERT_TRUE(stack.get_item_view(addrkey)->set(BytestringItem(payload)));
  finish_child(InvokeStatus<void>{}, false);

  auto const *root_data =
      stack.get_item_view(addrkey)->get<BytestringItem>()->bytes().data();

  for (int i = 0; i < 3; ++i) {
    ASSERT_TRUE(
        stack.start_frame(make_test_address(1), Calldata{}, 0, {}).has_value());
    auto *view = stack.get_item_view(addrkey);
    view->clear_flags();
    EXPECT_EQ(view->get<BytestringItem>()->bytes().data(), root_data);
    finish_child(InvokeStatus<void>{}, false);
  }

  // A sibling that overwrites the key and then reverts leaves the root's
  // bytestring as it was.
  ASSERT_TRUE(
      stack.start_frame(make_test_address(1), Calldata{}, 0, {}).has_value());
  ASSERT_TRUE(stack.get_item_view(addrkey)->set(BytestringItem(Bytestring{1})));
  stack.set_active_frame_return(
      InvocationFrameOutput(InvocationFrameStatusCode::Fault));
  finish_child(InvokeError{InvokeErrorCode::Deterministic}, true);

  auto const &root_item = *stack.get_item_view(addrkey)->get<BytestringItem>();
  EXPECT_EQ(root_item.bytes().data(), root_data);
  EXPECT_TRUE(std::ranges::equal(root_item.bytes(), payload));
}

// A frame numbers its forwarded tokens 0..N-1, in the order its parent listed
// them, whatever ids the parent held them under.
TEST_F(InvocationFrameTest, ForwardedTokensAreTheChildsFirstIds)
{
  InvocationFrameStack stack(nullptr, *per_tx_db_proxy_, kConfigParams);
  ASSERT_TRUE(
      stack.start_frame(make_test_address(0), Calldata{}, 0, {}).has_value());
  for (uint8_t consumer : {5, 6, 7}) {
    ASSERT_TRUE(mint(stack, make_token(consumer)).has_value());
  }
  auto const *a = node(stack, 0);
  auto const *b = node(stack, 1);
  auto const *c = node(stack, 2);

  ASSERT_TRUE(
      stack.start_frame(make_test_address(1), Calldata{}, 0, forward({2, 0}))
          .has_value());
  EXPECT_EQ(stack.get_current_frame_parent_supplied_auth_token_count(), 2U);
  EXPECT_EQ(node(stack, 0), c);
  EXPECT_EQ(node(stack, 1), a);
  EXPECT_EQ(node(stack, 2), nullptr);

  // The child mints after its forwarded tokens.
  auto minted = mint(stack, make_token(8));
  ASSERT_TRUE(minted.has_value());
  EXPECT_EQ(minted.value(), 2U);

  finish_child(stack);

  // The forwarded tokens left the parent; the rest keep their ids.
  EXPECT_EQ(node(stack, 0), nullptr);
  EXPECT_EQ(node(stack, 1), b);
  EXPECT_EQ(node(stack, 2), nullptr);
  EXPECT_EQ(stack.get_current_frame_parent_supplied_auth_token_count(), 0U);
}

// Tokens a child returns take the parent's next ids, in the order the child
// listed them, a returned forwarded token included.
TEST_F(InvocationFrameTest, ReturnedTokensTakeTheParentsNextIds)
{
  InvocationFrameStack stack(nullptr, *per_tx_db_proxy_, kConfigParams);
  ASSERT_TRUE(
      stack.start_frame(make_test_address(0), Calldata{}, 0, {}).has_value());
  ASSERT_TRUE(mint(stack, make_token(5)).has_value());
  ASSERT_TRUE(mint(stack, make_token(6)).has_value());
  auto const *a = node(stack, 0);
  auto const *b = node(stack, 1);

  ASSERT_TRUE(stack.start_frame(make_test_address(1), Calldata{}, 0, forward(1))
                  .has_value());
  auto minted = mint(stack, make_token(7));
  ASSERT_TRUE(minted.has_value());
  ASSERT_EQ(minted.value(), 1U);
  auto const *d = node(stack, 1);

  stack.set_active_frame_return(InvocationFrameOutput(
                                    InvocationFrameStatusCode::Return),
                                {1, 0});
  finish_child(stack);

  auto returned = stack.get_active_frame_child_returned_auth_tokens();
  EXPECT_EQ(returned.start, 2U);
  EXPECT_EQ(returned.count, 2U);
  EXPECT_EQ(node(stack, 0), a);
  EXPECT_EQ(node(stack, 1), nullptr);
  EXPECT_EQ(node(stack, 2), d);
  EXPECT_EQ(node(stack, 3), b);
}

// Consuming a token gives its children the frame's next ids, and they share
// their nodes with the consumed token.
TEST_F(InvocationFrameTest, ConsumeGivesChildrenTheFramesNextIds)
{
  InvocationFrameStack stack(nullptr, *per_tx_db_proxy_, kConfigParams);
  ASSERT_TRUE(
      stack.start_frame(make_test_address(0), Calldata{}, 0, {}).has_value());
  ASSERT_TRUE(mint(stack, make_token(5)).has_value());
  ASSERT_TRUE(mint(stack, make_token(1, {1, 1})).has_value());
  auto const *t = node(stack, 1);

  ASSERT_TRUE(stack.start_frame(make_test_address(1), Calldata{}, 0, forward(1))
                  .has_value());
  auto consumed = stack.consume_auth_token(0);
  ASSERT_TRUE(consumed.has_value());
  EXPECT_EQ(consumed->token.root_node.get(), t);
  EXPECT_EQ(consumed->token.mint_address, make_test_address(0));
  EXPECT_EQ(consumed->new_token_id_start, 1U);
  EXPECT_EQ(consumed->new_token_id_count, 2U);

  EXPECT_EQ(node(stack, 0), nullptr);
  EXPECT_EQ(node(stack, 1), t->child_nodes[0].get());
  EXPECT_EQ(node(stack, 2), t->child_nodes[1].get());
  EXPECT_EQ(stack.lookup_auth_token(1)->mint_address, make_test_address(0));
  EXPECT_EQ(stack.lookup_auth_token(1)->expiry_height, 100U);

  auto minted = mint(stack, make_token(9));
  ASSERT_TRUE(minted.has_value());
  EXPECT_EQ(minted.value(), 3U);
}

// The consume result keeps the consumed token alive after the frame drops it.
TEST_F(InvocationFrameTest, ConsumeResultHoldsTheConsumedToken)
{
  InvocationFrameStack stack(nullptr, *per_tx_db_proxy_, kConfigParams);
  ASSERT_TRUE(
      stack.start_frame(make_test_address(0), Calldata{}, 0, {}).has_value());
  ASSERT_TRUE(mint(stack, make_token(0)).has_value());

  auto consumed = stack.consume_auth_token(0);
  ASSERT_TRUE(consumed.has_value());
  EXPECT_EQ(node(stack, 0), nullptr);
  EXPECT_EQ(consumed->token.root_node.use_count(), 1U);
  EXPECT_EQ(consumed->token.root_node->payload, AuthTokenPayload{0});
}

// A child that consumes a forwarded token and then reverts leaves nothing
// behind: the parent keeps the token under its id, and a later child that
// consumes it numbers the children afresh.
TEST_F(InvocationFrameTest, RevertedConsumeLeavesNoTrace)
{
  InvocationFrameStack stack(nullptr, *per_tx_db_proxy_, kConfigParams);
  ASSERT_TRUE(
      stack.start_frame(make_test_address(0), Calldata{}, 0, {}).has_value());
  ASSERT_TRUE(mint(stack, make_token(1, {1, 1})).has_value());
  auto const *t = node(stack, 0);

  for (bool revert : {true, false}) {
    ASSERT_TRUE(
        stack.start_frame(make_test_address(1), Calldata{}, 0, forward(0))
            .has_value());
    auto consumed = stack.consume_auth_token(0);
    ASSERT_TRUE(consumed.has_value());
    EXPECT_EQ(consumed->new_token_id_start, 1U);
    EXPECT_EQ(consumed->new_token_id_count, 2U);
    if (revert) {
      stack.set_active_frame_return(
          InvocationFrameOutput(InvocationFrameStatusCode::Fault));
    }
    finish_child(stack, revert);
    EXPECT_EQ(node(stack, 0), revert ? t : nullptr);
  }

  // The parent's ids never moved.
  auto minted = mint(stack, make_token(2));
  ASSERT_TRUE(minted.has_value());
  EXPECT_EQ(minted.value(), 1U);
}

} // namespace hogd
