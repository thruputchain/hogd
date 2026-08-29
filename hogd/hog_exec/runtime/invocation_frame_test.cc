#include <gtest/gtest.h>

#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <utility>

#include "auth_token.hh"
#include "ffi_enums.hh"
#include "hog_exec_constants.hh"
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
  // These tests do not exercise the hash-buffer limit; any value admits the
  // frames they start.
  static constexpr uint32_t kMaxSha256Buffers = 8;

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
  ASSERT_TRUE(stack
                  .start_frame(make_test_address(0), Calldata{}, 0, {},
                               kMaxSha256Buffers)
                  .has_value());
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
                  .start_frame(make_test_address(1), Calldata{}, 0, {},
                               kMaxSha256Buffers)
                  .has_value());
  ASSERT_FALSE(
      stack.check_auth_tokens_owned(std::span<uint32_t>{&token_id.value(), 1}));
}

TEST_F(InvocationFrameTest, ForwardAuthToken)
{
  InvocationFrameStack stack(nullptr, *per_tx_db_proxy_);
  ASSERT_TRUE(stack
                  .start_frame(make_test_address(0), Calldata{}, 0, {},
                               kMaxSha256Buffers)
                  .has_value());
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
                               std::span<uint32_t>{&token_id.value(), 1},
                               kMaxSha256Buffers)
                  .has_value());
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
  ASSERT_TRUE(stack
                  .start_frame(make_test_address(0), Calldata{}, 0, {},
                               kMaxSha256Buffers)
                  .has_value());
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
                               std::span<uint32_t>{&token_id.value(), 1},
                               kMaxSha256Buffers)
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
  ASSERT_TRUE(stack
                  .start_frame(make_test_address(0), Calldata{}, 0, {},
                               kMaxSha256Buffers)
                  .has_value());

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
                               std::span<uint32_t>{&token_id.value(), 1},
                               kMaxSha256Buffers)
                  .has_value());
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
  ASSERT_TRUE(stack
                  .start_frame(make_test_address(0), Calldata{}, 0, {},
                               kMaxSha256Buffers)
                  .has_value());

  for (int i = 0; i < kChildCap; i++) {
    ASSERT_TRUE(stack
                    .start_frame(make_test_address(1), Calldata{},
                                 std::to_underlying(InvokeFlag::TryInvoke), {},
                                 kMaxSha256Buffers)
                    .has_value());
    auto result =
        stack.finish_frame(InvokeStatus<void>{}, execution_mode_data_);
    ASSERT_TRUE(result.has_value()) << "iteration " << i;
    ASSERT_TRUE(result.value().first.result_.has_value()) << "iteration " << i;
    ASSERT_EQ(result.value().second, false);
  }

  ASSERT_TRUE(stack
                  .start_frame(make_test_address(1), Calldata{},
                               std::to_underlying(InvokeFlag::TryInvoke), {},
                               kMaxSha256Buffers)
                  .has_value());
  auto result = stack.finish_frame(InvokeStatus<void>{}, execution_mode_data_);
  ASSERT_TRUE(result.has_value());
  ASSERT_FALSE(result.value().first.result_.has_value());
}

TEST_F(InvocationFrameTest, StartFrameReportsSuccess)
{
  // Frame admission is status-returning so that an allocation failure surfaces
  // as an Unrecoverable SystemError instead of terminating the executor across
  // SystemCallHandler::start_frame()'s noexcept boundary (#3058). On the normal
  // path it reports success, including when a token is forwarded into the
  // child.
  InvocationFrameStack stack(nullptr, *per_tx_db_proxy_);
  ASSERT_TRUE(stack
                  .start_frame(make_test_address(0), Calldata{}, 0, {},
                               kMaxSha256Buffers)
                  .has_value());

  AuthToken auth_token{.mint_address = make_test_address(0),
                       .expiry_height = 100,
                       .root_node = {.consumer_address = make_test_address(1),
                                     .payload = {},
                                     .child_nodes = {}}};
  auto token_id = stack.mint_auth_token(auth_token);
  ASSERT_TRUE(token_id.has_value());

  ASSERT_TRUE(stack
                  .start_frame(make_test_address(1), Calldata{}, 0,
                               std::span<uint32_t>{&token_id.value(), 1},
                               kMaxSha256Buffers)
                  .has_value());
  ASSERT_TRUE(
      stack.check_auth_tokens_owned(std::span<uint32_t>{&token_id.value(), 1}));
}

TEST_F(InvocationFrameTest, ManualRevertReclaimsThenReforward)
{
  InvocationFrameStack stack(nullptr, *per_tx_db_proxy_);
  ASSERT_TRUE(stack
                  .start_frame(make_test_address(0), Calldata{}, 0, {},
                               kMaxSha256Buffers)
                  .has_value());

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
                               std::span<uint32_t>{&token_id.value(), 1},
                               kMaxSha256Buffers)
                  .has_value());
  stack.set_active_frame_return(
      InvocationFrameOutput(InvocationFrameStatusCode::Fault));
  auto result1 = stack.finish_frame(InvokeStatus<void>{}, execution_mode_data_);
  ASSERT_TRUE(result1.has_value());
  ASSERT_EQ(result1.value().second, true);

  // token reclaimed by parent
  ASSERT_TRUE(
      stack.check_auth_tokens_owned(std::span<uint32_t>{&token_id.value(), 1}));

  // re-forward the same token to a new child that succeeds
  ASSERT_TRUE(stack
                  .start_frame(make_test_address(2), Calldata{}, 0,
                               std::span<uint32_t>{&token_id.value(), 1},
                               kMaxSha256Buffers)
                  .has_value());
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

// Distinct-key bytestring writes accumulate against a per-transaction budget
// and are rejected once the retained footprint would exceed the executor->HogDb
// message cap, rather than growing the frame without bound (issue #3681).
// Repeated writes to the same key net out to the current value and so do not
// consume the budget.
TEST_F(InvocationFrameTest, BytestringWriteBudgetBoundsRetainedFootprint)
{
  constexpr size_t kValueLen = 1024;

  InvocationFrameStack stack(nullptr, *per_tx_db_proxy_);
  ASSERT_TRUE(stack
                  .start_frame(make_test_address(0), Calldata{}, 0, {},
                               kMaxSha256Buffers)
                  .has_value());

  // Overwriting one key many times charges only the current value, so the
  // budget is barely touched.
  std::optional<size_t> prior;
  for (int i = 0; i < 1000; ++i) {
    ASSERT_TRUE(stack.try_reserve_bytestring_write(prior, kValueLen));
    prior = kValueLen;
  }

  // Distinct-key writes accumulate and are eventually rejected.
  size_t distinct_writes = 0;
  while (stack.try_reserve_bytestring_write(std::nullopt, kValueLen)) {
    ++distinct_writes;
    // The budget must bound the count; this guards against a non-terminating
    // loop if the accounting ever regresses.
    ASSERT_LT(distinct_writes, size_t{1} << 20);
  }

  // The cap is the ~16 MiB executor->HogDb message cap, so roughly
  // 16 MiB / (value + per-entry overhead) 1 KiB writes fit -- far more than any
  // honest transaction needs, but firmly bounded.
  EXPECT_GT(distinct_writes, size_t{15000});
  EXPECT_LT(distinct_writes, size_t{17000});
}

// A frame that reverts discards its writes, so its charge against the budget is
// rolled back and the freed footprint is available to the parent again.
TEST_F(InvocationFrameTest, RevertedFrameUnchargesBytestringWriteBudget)
{
  constexpr size_t kValueLen = 1024;

  InvocationFrameStack stack(nullptr, *per_tx_db_proxy_);
  ASSERT_TRUE(stack
                  .start_frame(make_test_address(0), Calldata{}, 0, {},
                               kMaxSha256Buffers)
                  .has_value());

  // A child frame exhausts the budget with distinct-key writes, then reverts.
  ASSERT_TRUE(stack
                  .start_frame(make_test_address(1), Calldata{}, 0, {},
                               kMaxSha256Buffers)
                  .has_value());
  while (stack.try_reserve_bytestring_write(std::nullopt, kValueLen)) {
  }
  ASSERT_FALSE(stack.try_reserve_bytestring_write(std::nullopt, kValueLen));

  stack.set_active_frame_return(
      InvocationFrameOutput(InvocationFrameStatusCode::Fault));
  auto result = stack.finish_frame(InvokeError{InvokeErrorCode::Deterministic},
                                   execution_mode_data_);
  ASSERT_TRUE(result.has_value());
  ASSERT_FALSE(result.value().first.result_.has_value());

  // The reverted child's writes were uncharged, so the parent can write again.
  EXPECT_TRUE(stack.try_reserve_bytestring_write(std::nullopt, kValueLen));
}

// Replacing a bytestring at a key with another item type (a delete or an nnint
// write) releases its charge, so a contract that alternates a bytestring write
// with another type on one key cannot leak the budget (issue #3681).
TEST_F(InvocationFrameTest, ReleasingBytestringWriteReclaimsBudget)
{
  constexpr size_t kValueLen = 1024;

  InvocationFrameStack stack(nullptr, *per_tx_db_proxy_);
  ASSERT_TRUE(stack
                  .start_frame(make_test_address(0), Calldata{}, 0, {},
                               kMaxSha256Buffers)
                  .has_value());

  // Far more write/replace cycles on one key than would fit if the charge
  // leaked; each write is followed by the release the replacing write performs.
  for (int i = 0; i < 100'000; ++i) {
    ASSERT_TRUE(stack.try_reserve_bytestring_write(std::nullopt, kValueLen));
    stack.release_bytestring_write(kValueLen);
  }

  // The budget is fully reclaimed, so a large batch of distinct writes still
  // fits.
  size_t distinct_writes = 0;
  while (stack.try_reserve_bytestring_write(std::nullopt, kValueLen)) {
    ++distinct_writes;
    ASSERT_LT(distinct_writes, size_t{1} << 20);
  }
  EXPECT_GT(distinct_writes, size_t{15000});
}

} // namespace hogd
