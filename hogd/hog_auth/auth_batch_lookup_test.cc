#include <gtest/gtest.h>

#include <atomic>
#include <format>
#include <map>
#include <memory>
#include <mutex>
#include <span>
#include <utility>
#include <vector>

#include "address_auth.hh"
#include "auth_card.hh"
#include "auth_claim.hh"
#include "auth_rpc.hh"
#include "hogd/base/coropoll.h"
#include "hogd/base/log.h"
#include "hogd/base/rpc_server.h"
#include "hogd/base/types.h"
#include "hogd/hog_auth/auth_server.h"
#include "hogd/hog_auth/dbauth_client.h"
#include "rpc_types.hh"
#include "transaction.hh"
#include "types.hh"
#include "x23/async.h"
#include "x23/corosync.h"
#include "x23/x23.h"

namespace hogd {
namespace {

class BatchDBAuthRpc {
public:
  using rpc_service = DBAuth1;

  void set(ContractAddress address, DBAuthResult result)
  {
    std::lock_guard lock(mutex_);
    replies_[address] = std::move(result);
  }

  DBAuthResult authdb_get_auth_data(BlockHeight /*height*/,
                                    ContractAddress /*address*/)
  {
    std::lock_guard lock(mutex_);
    ++single_calls_;
    return DBAuthResult(DBAuthError::kDBAuthInternal);
  }

  x23::Async<DBAuthResultBatch>
  authdb_get_auth_data_batch(BlockHeight /*height*/,
                             ContractAddressBatch addresses)
  {
    DBAuthResultBatch result;
    {
      std::lock_guard lock(mutex_);
      batch_sizes_.push_back(addresses.size());
      for (const ContractAddress &address : addresses) {
        auto it = replies_.find(address);
        result.push_back(it == replies_.end()
                             ? DBAuthResult(DBAuthError::kDBAuthNotFound)
                             : it->second);
      }
      if (truncate_reply_ && !result.empty()) {
        result.pop_back();
      }
    }
    batch_started_.store(true);
    batch_started_.notify_all();
    while (!release_reply_.load()) {
      co_await release_reply_.wait(false);
    }
    co_return result;
  }

  static DBAuthIntroResult dbauth_intro(const AuthCard & /*card*/)
  {
    DBAuthIntroResult result(DBAuthIntroOutcome::kIntroSuccess);
    result.current_height() = 0;
    return result;
  }

  void truncate_reply(bool truncate)
  {
    std::lock_guard lock(mutex_);
    truncate_reply_ = truncate;
  }

  void delay_reply() { release_reply_.store(false); }
  void wait_for_batch() { batch_started_.wait(false); }
  void release_reply()
  {
    release_reply_.store(true);
    release_reply_.broadcast(&default_threadpool());
  }

  std::pair<size_t, std::vector<size_t>> calls()
  {
    std::lock_guard lock(mutex_);
    return {single_calls_, batch_sizes_};
  }

private:
  std::mutex mutex_;
  std::map<ContractAddress, DBAuthResult> replies_;
  size_t single_calls_ = 0;
  std::vector<size_t> batch_sizes_;
  bool truncate_reply_ = false;
  std::atomic<bool> batch_started_ = false;
  x23::CoroFutex<bool> release_reply_{true};
};

static_assert(is_DBAuth1<BatchDBAuthRpc *, x23::MaybeAsync>);

ContractAddress
address_for(size_t id)
{
  ContractAddress address{};
  address[0] = static_cast<uint8_t>(id);
  address[1] = static_cast<uint8_t>(id >> 8);
  return address;
}

SignedTransaction
unsigned_tx(ContractAddress address)
{
  SignedTransaction tx{};
  tx.tx.invocation.invoked_address = address;
  tx.tx.auth_claim = AuthClaim(AuthClaimType::kSourceAddressAuthData);
  return tx;
}

class AuthBatchLookupTest : public ::testing::Test {
protected:
  AuthBatchLookupTest()
  {
    const uint16_t port = db_.start_server(0);
    auto client = make_dbauth_client(std::format("localhost:{}", port))();
    CHECK(client.has_value());
    auth_ =
        std::make_unique<AuthServer>(std::move(*client), NetworkHash{}, 4096);
    auth_->register_auth_data_updates(0, KeyChangeBatch{})();
  }

  BatchDBAuthRpc &db() { return *db_.impl(); }

  void set_zero_threshold(ContractAddress address)
  {
    DBAuthResult result(DBAuthError::kDBAuthSuccess);
    result.auth_data() = AddressAuthData{};
    db().set(address, std::move(result));
  }

  AuthErrorCodeBatch verify(const SignedTransactionBatch &txs,
                            BlockHeight height = 0)
  {
    return auth_->verify_transactions(
        height, std::span<const SignedTransaction>(txs.data(), txs.size()),
        4)();
  }

  RpcServer<BatchDBAuthRpc> db_;
  std::unique_ptr<AuthServer> auth_;
};

TEST_F(AuthBatchLookupTest, DeduplicatesAndBoundsColdLookups)
{
  constexpr size_t kAddresses = DBAuthClient::kMaxLookupBatchSize + 3;
  SignedTransactionBatch txs;
  for (size_t i = 0; i < kAddresses; ++i) {
    const auto address = address_for(i);
    set_zero_threshold(address);
    txs.push_back(unsigned_tx(address));
    txs.push_back(unsigned_tx(address));
  }
  for (AuthErrorCode result : verify(txs)) {
    EXPECT_EQ(result, AuthErrorCode::kAuthSuccess);
  }
  const auto [single_calls, batch_sizes] = db().calls();
  EXPECT_EQ(single_calls, 0U);
  EXPECT_EQ(batch_sizes,
            (std::vector<size_t>{DBAuthClient::kMaxLookupBatchSize, 3}));

  // The same request is now entirely cached.
  for (AuthErrorCode result : verify(txs)) {
    EXPECT_EQ(result, AuthErrorCode::kAuthSuccess);
  }
  EXPECT_EQ(db().calls().second, batch_sizes);
}

TEST_F(AuthBatchLookupTest, PreservesOrderAndDoesNotCacheErrors)
{
  set_zero_threshold(address_for(1));
  db().set(address_for(3), DBAuthResult(DBAuthError::kDBAuthStaleBlockHeight));
  db().set(address_for(4), DBAuthResult(DBAuthError::kDBAuthInternal));
  SignedTransactionBatch txs;
  for (size_t id : {1U, 2U, 3U, 4U, 1U, 4U}) {
    txs.push_back(unsigned_tx(address_for(id)));
  }
  const AuthErrorCodeBatch expected{AuthErrorCode::kAuthSuccess,
                                    AuthErrorCode::kAuthFailed,
                                    AuthErrorCode::kAuthStaleBlockHeight,
                                    AuthErrorCode::kAuthInternal,
                                    AuthErrorCode::kAuthSuccess,
                                    AuthErrorCode::kAuthInternal};
  EXPECT_EQ(verify(txs), expected);
  EXPECT_EQ(db().calls().second, (std::vector<size_t>{4}));

  set_zero_threshold(address_for(3));
  set_zero_threshold(address_for(4));
  const auto retried = verify(txs);
  ASSERT_EQ(retried.size(), txs.size());
  for (size_t i = 0; i < retried.size(); ++i) {
    EXPECT_EQ(retried[i], i == 1 ? AuthErrorCode::kAuthFailed
                                 : AuthErrorCode::kAuthSuccess);
  }
  // Positive and negative entries stay cached; only errors are fetched again.
  EXPECT_EQ(db().calls().second, (std::vector<size_t>{4, 2}));
}

TEST_F(AuthBatchLookupTest, OldHeightUsesOnlyUnchangedCachedEntries)
{
  set_zero_threshold(address_for(1));
  EXPECT_EQ(verify({unsigned_tx(address_for(1))})[0],
            AuthErrorCode::kAuthSuccess);
  KeyChangeBatch changes;
  changes.push_back(
      KeyChange{.address = address_for(2),
                .auth_data = x23::Ptr<AddressAuthData>(std::in_place)});
  auth_->register_auth_data_updates(1, std::move(changes))();

  EXPECT_EQ(verify({unsigned_tx(address_for(1)), unsigned_tx(address_for(2)),
                    unsigned_tx(address_for(3))}),
            (AuthErrorCodeBatch{AuthErrorCode::kAuthSuccess,
                                AuthErrorCode::kAuthStaleBlockHeight,
                                AuthErrorCode::kAuthStaleBlockHeight}));
  EXPECT_EQ(db().calls().second, (std::vector<size_t>{1}));
}

TEST_F(AuthBatchLookupTest, WrongReplyLengthFailsWithoutCaching)
{
  set_zero_threshold(address_for(1));
  db().truncate_reply(true);
  const SignedTransactionBatch txs{unsigned_tx(address_for(1))};
  EXPECT_EQ(verify(txs)[0], AuthErrorCode::kAuthInternal);
  db().truncate_reply(false);
  EXPECT_EQ(verify(txs)[0], AuthErrorCode::kAuthSuccess);
  EXPECT_EQ(db().calls().second, (std::vector<size_t>{1, 1}));
}

TEST_F(AuthBatchLookupTest, DelayedReplyCannotReplaceANewerCacheEntry)
{
  const auto address = address_for(1);
  set_zero_threshold(address);
  db().delay_reply();
  const SignedTransactionBatch txs{unsigned_tx(address)};
  auto pending = auth_->verify_transactions(
      0, std::span<const SignedTransaction>(txs.data(), txs.size()), 1);
  db().wait_for_batch();

  AddressAuthData newer{};
  newer.required_weight = 1;
  KeyChangeBatch changes;
  changes.push_back(KeyChange{.address = address,
                              .auth_data = x23::Ptr<AddressAuthData>(newer)});
  auth_->register_auth_data_updates(1, std::move(changes))();
  db().release_reply();

  // The in-flight request owns the old snapshot. Later requests see the update.
  EXPECT_EQ(pending()[0], AuthErrorCode::kAuthSuccess);
  EXPECT_EQ(verify(txs, 1)[0], AuthErrorCode::kAuthFailed);
  EXPECT_EQ(db().calls().second, (std::vector<size_t>{1}));
}

} // namespace
} // namespace hogd
