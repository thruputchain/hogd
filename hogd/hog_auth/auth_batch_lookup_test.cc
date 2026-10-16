#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <format>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

#include "address_auth.hh"
#include "auth_card.hh"
#include "auth_claim.hh"
#include "auth_rpc.hh"
#include "hogd/base/coropoll.h"
#include "hogd/base/log.h"
#include "hogd/base/rpc_client.h"
#include "hogd/base/rpc_server.h"
#include "hogd/base/types.h"
#include "hogd/hog_auth/auth_server.h"
#include "hogd/hog_auth/dbauth_client.h"
#include "hogd/hog_auth/status.h"
#include "rpc_types.hh"
#include "transaction.hh"
#include "types.hh"
#include "x23/async.h"
#include "x23/corosync.h"
#include "x23/serialize.h"
#include "x23/sockio.h"
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
    size_t batch;
    bool fail;
    {
      std::lock_guard lock(mutex_);
      batch = batch_sizes_.size();
      batch_sizes_.push_back(addresses.size());
      max_in_flight_ = std::max(max_in_flight_, ++in_flight_);
      fail = fail_address_.has_value() &&
             std::ranges::find(addresses, *fail_address_) != addresses.end();
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
    progress_.notify_all();
    for (;;) {
      const size_t first_released = first_released_.load();
      if (batch >= first_released) {
        break;
      }
      co_await first_released_.wait(first_released);
    }
    {
      std::lock_guard lock(mutex_);
      --in_flight_;
      ++completed_;
    }
    progress_.notify_all();
    if (fail) {
      throw std::runtime_error("injected DB auth RPC failure");
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

  void fail_address(std::optional<ContractAddress> address)
  {
    std::lock_guard lock(mutex_);
    fail_address_ = std::move(address);
  }

  void delay_reply()
  {
    first_released_.store(std::numeric_limits<size_t>::max());
  }
  bool wait_for_batches(size_t count)
  {
    std::unique_lock lock(mutex_);
    return progress_.wait_for(lock, std::chrono::seconds(5),
                              [&] { return batch_sizes_.size() >= count; });
  }
  bool wait_for_replies(size_t count)
  {
    std::unique_lock lock(mutex_);
    return progress_.wait_for(lock, std::chrono::seconds(5),
                              [&] { return completed_ >= count; });
  }
  void release_reply(size_t first_batch = 0)
  {
    first_released_.store(first_batch);
    first_released_.broadcast(&default_threadpool());
  }

  size_t max_in_flight()
  {
    std::lock_guard lock(mutex_);
    return max_in_flight_;
  }

  std::pair<size_t, std::vector<size_t>> calls()
  {
    std::lock_guard lock(mutex_);
    return {single_calls_, batch_sizes_};
  }

private:
  std::mutex mutex_;
  std::condition_variable progress_;
  std::map<ContractAddress, DBAuthResult> replies_;
  size_t single_calls_ = 0;
  std::vector<size_t> batch_sizes_;
  bool truncate_reply_ = false;
  std::optional<ContractAddress> fail_address_;
  size_t in_flight_ = 0;
  size_t max_in_flight_ = 0;
  size_t completed_ = 0;
  x23::CoroFutex<size_t> first_released_{0};
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
    port_ = db_.start_server(0);
    auto client = make_dbauth_client(std::format("localhost:{}", port_))();
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
  uint16_t port_;
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
  auto sorted_sizes = batch_sizes;
  std::ranges::sort(sorted_sizes);
  EXPECT_EQ(sorted_sizes,
            (std::vector<size_t>{3, DBAuthClient::kMaxLookupBatchSize}));

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

TEST_F(AuthBatchLookupTest, OverlapsBoundedLookupsAndPreservesReplyOrder)
{
  constexpr size_t kWindow = AuthServer::kMaxLookupBatchesInFlight;
  constexpr size_t kAddresses =
      (DBAuthClient::kMaxLookupBatchSize * (kWindow + 1)) + 3;
  SignedTransactionBatch txs;
  AuthErrorCodeBatch expected;
  for (size_t i = 0; i < kAddresses; ++i) {
    const auto address = address_for(i);
    AuthErrorCode code = AuthErrorCode::kAuthSuccess;
    if (i % 7 == 0) {
      db().set(address, DBAuthResult(DBAuthError::kDBAuthInternal));
      code = AuthErrorCode::kAuthInternal;
    }
    else if (i % 7 == 1) {
      code = AuthErrorCode::kAuthFailed;
    }
    else {
      set_zero_threshold(address);
    }
    txs.push_back(unsigned_tx(address));
    txs.push_back(unsigned_tx(address));
    expected.push_back(code);
    expected.push_back(code);
  }
  db().delay_reply();
  auto pending = auth_->verify_transactions(
      0, std::span<const SignedTransaction>(txs.data(), txs.size()), 4);
  const bool overlapped = db().wait_for_batches(kWindow);
  const size_t initially_started = db().calls().second.size();
  // Let later RPCs finish while the first one remains blocked.
  db().release_reply(1);
  const bool finished_out_of_order = db().wait_for_replies(kWindow - 1);
  db().release_reply();
  const auto results = pending();

  EXPECT_TRUE(overlapped);
  EXPECT_TRUE(finished_out_of_order);
  EXPECT_EQ(initially_started, kWindow);
  EXPECT_EQ(db().max_in_flight(), kWindow);
  EXPECT_EQ(results, expected);
  const auto [single_calls, batch_sizes] = db().calls();
  EXPECT_EQ(single_calls, 0U);
  EXPECT_EQ(batch_sizes.size(), kWindow + 2);
  EXPECT_EQ(std::ranges::count(batch_sizes, DBAuthClient::kMaxLookupBatchSize),
            kWindow + 1);
  EXPECT_EQ(std::ranges::count(batch_sizes, 3U), 1);
}

TEST_F(AuthBatchLookupTest, RpcFailureDoesNotDiscardOtherBatches)
{
  constexpr size_t kBatchSize = DBAuthClient::kMaxLookupBatchSize;
  SignedTransactionBatch txs;
  for (size_t i = 0; i < 3 * kBatchSize; ++i) {
    set_zero_threshold(address_for(i));
    txs.push_back(unsigned_tx(address_for(i)));
  }
  db().fail_address(address_for(kBatchSize));
  const auto results = verify(txs);
  ASSERT_EQ(results.size(), txs.size());
  for (size_t i = 0; i < results.size(); ++i) {
    EXPECT_EQ(results[i], i >= kBatchSize && i < 2 * kBatchSize
                              ? AuthErrorCode::kAuthInternal
                              : AuthErrorCode::kAuthSuccess);
  }
  db().fail_address(std::nullopt);
  for (AuthErrorCode result : verify(txs)) {
    EXPECT_EQ(result, AuthErrorCode::kAuthSuccess);
  }
  // Only the failed batch needs another DB lookup.
  EXPECT_EQ(db().calls().second, (std::vector<size_t>(4, kBatchSize)));
}

TEST_F(AuthBatchLookupTest, RespectsConfiguredReplySizeLimit)
{
  DBAuthResult full(DBAuthError::kDBAuthSuccess);
  full.auth_data() = AddressAuthData{};
  for (auto &key : full.auth_data().public_keys) {
    key = x23::Ptr<PublicKeyConfig>(std::in_place);
    key->key = PublicKey(CryptographicScheme::MlDsa65);
    key->key.mldsa65()->fill(0x5a);
  }
  // This reader can fit two worst-case accounts, but not three.
  const size_t cap = 1024 + (2 * x23::xdr_size(full));
  x23::HostPort host_port;
  ASSERT_TRUE(host_port.parse(std::format("localhost:{}", port_)));
  auto client = make_rpc_client<DBAuth1>(host_port, false, cap)();
  ASSERT_TRUE(client.has_value());
  auth_ = std::make_unique<AuthServer>(std::move(*client), NetworkHash{}, 4096);
  auth_->register_auth_data_updates(0, KeyChangeBatch{})();
  SignedTransactionBatch txs;
  for (size_t i = 0; i < 5; ++i) {
    db().set(address_for(i), full);
    txs.push_back(unsigned_tx(address_for(i)));
  }
  for (AuthErrorCode result : verify(txs)) {
    EXPECT_EQ(result, AuthErrorCode::kAuthSuccess);
  }
  auto batch_sizes = db().calls().second;
  std::ranges::sort(batch_sizes);
  EXPECT_EQ(batch_sizes, (std::vector<size_t>{1, 2, 2}));
}

TEST_F(AuthBatchLookupTest, OldHeightUsesOnlyUnchangedCachedEntries)
{
  set_zero_threshold(address_for(1));
  EXPECT_EQ(verify({unsigned_tx(address_for(1))})[0],
            AuthErrorCode::kAuthSuccess);
  EXPECT_EQ(verify({unsigned_tx(address_for(4))})[0],
            AuthErrorCode::kAuthFailed);
  KeyChangeBatch changes;
  changes.push_back(
      KeyChange{.address = address_for(2),
                .auth_data = x23::Ptr<AddressAuthData>(std::in_place)});
  auth_->register_auth_data_updates(1, std::move(changes))();

  EXPECT_EQ(verify({unsigned_tx(address_for(1)), unsigned_tx(address_for(2)),
                    unsigned_tx(address_for(3)), unsigned_tx(address_for(4))}),
            (AuthErrorCodeBatch{AuthErrorCode::kAuthSuccess,
                                AuthErrorCode::kAuthStaleBlockHeight,
                                AuthErrorCode::kAuthStaleBlockHeight,
                                AuthErrorCode::kAuthFailed}));
  // The single-account path applies the same positive/negative cache and
  // stale-height rules without going back to the DB.
  const auto cached = auth_->lookup_auth_data(0, address_for(1))();
  ASSERT_TRUE(cached.has_value());
  ASSERT_NE(*cached, nullptr);
  EXPECT_TRUE((*cached)->second.has_value());
  const auto missing = auth_->lookup_auth_data(0, address_for(4))();
  ASSERT_TRUE(missing.has_value());
  ASSERT_NE(*missing, nullptr);
  EXPECT_FALSE((*missing)->second.has_value());
  for (size_t id : {2U, 3U}) {
    const auto stale = auth_->lookup_auth_data(0, address_for(id))();
    ASSERT_FALSE(stale.has_value());
    EXPECT_EQ(stale.error().code(), AuthErrorCode::kAuthStaleBlockHeight);
  }
  EXPECT_EQ(db().calls().first, 0U);
  EXPECT_EQ(db().calls().second, (std::vector<size_t>{1, 1}));
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
  const bool started = db().wait_for_batches(1);

  AddressAuthData newer{};
  newer.required_weight = 1;
  KeyChangeBatch changes;
  changes.push_back(KeyChange{.address = address,
                              .auth_data = x23::Ptr<AddressAuthData>(newer)});
  auth_->register_auth_data_updates(1, std::move(changes))();
  db().release_reply();

  // The in-flight request owns the old snapshot. Later requests see the update.
  EXPECT_EQ(pending()[0], AuthErrorCode::kAuthSuccess);
  EXPECT_TRUE(started);
  EXPECT_EQ(verify(txs, 1)[0], AuthErrorCode::kAuthFailed);
  EXPECT_EQ(db().calls().second, (std::vector<size_t>{1}));
}

} // namespace
} // namespace hogd
