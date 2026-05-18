#include <gtest/gtest.h>

#include <memory>
#include <span>
#include <utility>

#include "auth_token.hh"
#include "ffi_enums.hh"
#include "hogd/base/types.h"
#include "hogd/hogexec/common/status.h"
#include "hogd/hogexec/components/execution_mode.h"
#include "hogd/hogexec/runtime/invocation_frame.h"
#include "hogd/hogexec/runtime/item/database_proxy.h"
#include "hogd/hogexec/runtime/test_util/block_snapshot_server.h"
#include "hogexec_constants.hh"
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
  InvocationFrameStack stack(nullptr, *per_tx_db_proxy_);
  stack.start_frame(make_test_address(0), Calldata{}, 0, {});
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

  stack.start_frame(make_test_address(1), Calldata{}, 0, {});
  ASSERT_FALSE(
      stack.check_auth_tokens_owned(std::span<uint32_t>{&token_id.value(), 1}));
}

TEST_F(InvocationFrameTest, ForwardAuthToken)
{
  InvocationFrameStack stack(nullptr, *per_tx_db_proxy_);
  stack.start_frame(make_test_address(0), Calldata{}, 0, {});
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

  stack.start_frame(make_test_address(1), Calldata{}, 0,
                    std::span<uint32_t>{&token_id.value(), 1});
  ASSERT_TRUE(
      stack.check_auth_tokens_owned(std::span<uint32_t>{&token_id.value(), 1}));

  // frame commits successfully
  auto result = stack.finish_frame(InvokeStatus<void>{}, execution_mode_data_);
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
  InvocationFrameStack stack(nullptr, *per_tx_db_proxy_);
  stack.start_frame(make_test_address(0), Calldata{}, 0, {});
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

  stack.start_frame(make_test_address(1), Calldata{}, 0,
                    std::span<uint32_t>{&token_id.value(), 1});
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
                                   execution_mode_data_);
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
  InvocationFrameStack stack(nullptr, *per_tx_db_proxy_);
  stack.start_frame(make_test_address(0), Calldata{}, 0, {});

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
  stack.start_frame(make_test_address(1), Calldata{}, 0,
                    std::span<uint32_t>{&token_id.value(), 1});
  ASSERT_TRUE(
      stack.check_auth_tokens_owned(std::span<uint32_t>{&token_id.value(), 1}));

  // child manually reverts: invoke succeeds but frame output is Fault
  stack.set_active_frame_return(
      InvocationFrameOutput(InvocationFrameStatusCode::Fault));
  auto result = stack.finish_frame(InvokeStatus<void>{}, execution_mode_data_);
  ASSERT_TRUE(result.has_value());
  ASSERT_TRUE(result.value().first.result_.has_value());
  ASSERT_EQ(result.value().second, true);

  // parent must own the token again
  ASSERT_TRUE(
      stack.check_auth_tokens_owned(std::span<uint32_t>{&token_id.value(), 1}));
  ASSERT_NE(stack.lookup_auth_token(token_id.value()), nullptr);
}

TEST_F(InvocationFrameTest, TryInvokeHitsSavepointDepthLimit)
{
  // One slot is reserved for root commit, so the child cap is
  // kMaxSavepointDepth - 1.
  static constexpr int kChildCap = kMaxSavepointDepth - 1;

  InvocationFrameStack stack(nullptr, *per_tx_db_proxy_);
  stack.start_frame(make_test_address(0), Calldata{}, 0, {});

  for (int i = 0; i < kChildCap; i++) {
    stack.start_frame(make_test_address(1), Calldata{},
                      std::to_underlying(InvokeFlag::TryInvoke), {});
    auto result =
        stack.finish_frame(InvokeStatus<void>{}, execution_mode_data_);
    ASSERT_TRUE(result.has_value()) << "iteration " << i;
    ASSERT_TRUE(result.value().first.result_.has_value()) << "iteration " << i;
    ASSERT_EQ(result.value().second, false);
  }

  stack.start_frame(make_test_address(1), Calldata{},
                    std::to_underlying(InvokeFlag::TryInvoke), {});
  auto result = stack.finish_frame(InvokeStatus<void>{}, execution_mode_data_);
  ASSERT_TRUE(result.has_value());
  ASSERT_FALSE(result.value().first.result_.has_value());
}

TEST_F(InvocationFrameTest, ManualRevertReclaimsThenReforward)
{
  InvocationFrameStack stack(nullptr, *per_tx_db_proxy_);
  stack.start_frame(make_test_address(0), Calldata{}, 0, {});

  AuthToken auth_token{.mint_address = make_test_address(0),
                       .expiry_height = 100,
                       .root_node = {.consumer_address = make_test_address(1),
                                     .payload = {},
                                     .child_nodes = {}}};

  auto token_id = stack.mint_auth_token(auth_token);
  ASSERT_TRUE(token_id.has_value());

  // forward token to child, which manually reverts
  stack.start_frame(make_test_address(1), Calldata{}, 0,
                    std::span<uint32_t>{&token_id.value(), 1});
  stack.set_active_frame_return(
      InvocationFrameOutput(InvocationFrameStatusCode::Fault));
  auto result1 = stack.finish_frame(InvokeStatus<void>{}, execution_mode_data_);
  ASSERT_TRUE(result1.has_value());
  ASSERT_EQ(result1.value().second, true);

  // token reclaimed by parent
  ASSERT_TRUE(
      stack.check_auth_tokens_owned(std::span<uint32_t>{&token_id.value(), 1}));

  // re-forward the same token to a new child that succeeds
  stack.start_frame(make_test_address(2), Calldata{}, 0,
                    std::span<uint32_t>{&token_id.value(), 1});
  ASSERT_TRUE(
      stack.check_auth_tokens_owned(std::span<uint32_t>{&token_id.value(), 1}));

  auto result2 = stack.finish_frame(InvokeStatus<void>{}, execution_mode_data_);
  ASSERT_TRUE(result2.has_value());
  ASSERT_TRUE(result2.value().first.result_.has_value());
  ASSERT_EQ(result2.value().second, false);

  // parent no longer owns the token (successful child keeps it)
  ASSERT_FALSE(
      stack.check_auth_tokens_owned(std::span<uint32_t>{&token_id.value(), 1}));
}

} // namespace hogd
