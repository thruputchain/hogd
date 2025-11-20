#pragma once

#include <map>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "account.hh"
#include "auth_card.hh"
#include "auth_rpc.hh"
#include "hogd/base/coropoll.h"
#include "hogd/base/rpc_server.h"
#include "hogd/base/xdr_util.h" // IWYU pragma: keep
#include "types.hh"
#include "x23/async.h"
#include "x23/x23.h"

namespace hogd {

// Mock implementation of DBAuth1 RPC interface that tracks auth state and
// computes diffs when height is incremented, storing them as KeyChangeBatch.
class MockDBAuthRpc {
public:
  using rpc_service = DBAuth1;

  MockDBAuthRpc() = default;

  // Get the current block height
  BlockHeight get_current_height() const { return current_height_; }

  // Set auth data for an address (modifies current state)
  void set(ContractAddress address, const AccountAuthData &auth_data)
  {
    std::lock_guard<std::mutex> lock(mutex_);
    current_auth_state_[address] = auth_data;
    x23::Ptr<AccountAuthData> auth_data_ptr;
    auth_data_ptr.emplace(current_auth_state_[address]);
    current_key_changes_[address] =
        KeyChange{.address = address, .auth_data = std::move(auth_data_ptr)};
  }

  // Delete auth data for an address (modifies current state)
  void delete_address(ContractAddress address)
  {
    std::lock_guard<std::mutex> lock(mutex_);
    current_auth_state_.erase(address);
    current_key_changes_[address] =
        KeyChange{.address = address, .auth_data = nullptr};
  }

  // Increment the block height, computing and storing the diff as
  // KeyChangeBatch
  void increment_height()
  {
    std::lock_guard<std::mutex> lock(mutex_);
    BlockHeight new_height = current_height_ + 1;

    KeyChangeBatch changes;
    for (const auto &[_, auth_data] : current_key_changes_) {
      changes.push_back(auth_data);
    }

    // Store the changes for this height
    if (!changes.empty()) {
      changes_by_height_[new_height] = std::move(changes);
    }
    current_height_ = new_height;
  }

  // Materialize the KeyChangeBatch for a particular block height
  KeyChangeBatch get_key_changes_at_height(BlockHeight height) const
  {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = changes_by_height_.find(height);
    if (it == changes_by_height_.end()) {
      return KeyChangeBatch{};
    }
    return it->second;
  }

  void address_should_be_cached(ContractAddress address)
  {
    std::lock_guard<std::mutex> lock(mutex_);
    addresses_that_should_be_cached_.insert(address);
  }

  // RPC methods
  x23::Async<DBAuthResult> authdb_get_auth_data(BlockHeight height,
                                                ContractAddress address);

  x23::Async<DBAuthResultBatch>
  authdb_get_auth_data_batch(BlockHeight height,
                             ContractAddressBatch addresses);

  static x23::Async<void> dbauth_intro(AuthCard card) { co_return; }

private:
  // Find the most recent auth data for an address at or before the given height
  std::optional<AccountAuthData>
  find_auth_data_at_height(ContractAddress address, BlockHeight height) const;

  void check_should_be_cached(ContractAddress address) const
  {
    if (addresses_that_should_be_cached_.contains(address)) {
      throw std::runtime_error("Address should be cached but is not");
    }
  }

  std::unordered_map<ContractAddress, AccountAuthData> current_auth_state_;
  std::unordered_map<ContractAddress, KeyChange> current_key_changes_;

  std::unordered_set<ContractAddress> addresses_that_should_be_cached_;

  // Map from block height to the key changes made at that height
  std::map<BlockHeight, KeyChangeBatch> changes_by_height_;
  mutable std::mutex mutex_;
  BlockHeight current_height_ = 0;
};

static_assert(is_DBAuth1<MockDBAuthRpc *, x23::Async>);

class MockDBAuthServer : public RpcServer<MockDBAuthRpc> {
public:
  MockDBAuthServer() : RpcServer(default_coropoll()) {}
};

} // namespace hogd
