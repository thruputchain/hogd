#pragma once

#include <atomic>
#include <cstddef>
#include <map>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "address_auth.hh"
#include "auth_card.hh"
#include "auth_rpc.hh"
#include "hogd/base/coropoll.h"
#include "hogd/base/log.h"
#include "hogd/base/rpc_server.h"
#include "hogd/base/thread_checked_mutex.h"
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
  void set(ContractAddress address, const AddressAuthData &auth_data)
  {
    std::scoped_lock lock(mutex_);
    current_auth_state_[address] = auth_data;
    x23::Ptr<AddressAuthData> auth_data_ptr;
    auth_data_ptr.emplace(current_auth_state_[address]);
    current_key_changes_[address] =
        KeyChange{.address = address, .auth_data = std::move(auth_data_ptr)};
  }

  // Delete auth data for an address (modifies current state)
  void delete_address(ContractAddress address)
  {
    std::scoped_lock lock(mutex_);
    current_auth_state_.erase(address);
    current_key_changes_[address] =
        KeyChange{.address = address, .auth_data = nullptr};
  }

  // Increment the block height, computing and storing the diff as
  // KeyChangeBatch
  void increment_height()
  {
    std::scoped_lock lock(mutex_);
    BlockHeight new_height = current_height_ + 1;

    compute_key_changes_for_current_height();
    current_key_changes_.clear();

    current_height_ = new_height;
  }

  void compute_key_changes_for_current_height()
  {
    KeyChangeBatch changes;
    for (const auto &[_, auth_data] : current_key_changes_) {
      changes.push_back(auth_data);
    }

    // Store the changes for this height
    if (!changes.empty()) {
      changes_by_height_[current_height_] = std::move(changes);
    }
  }

  // Materialize the KeyChangeBatch for a particular block height
  KeyChangeBatch get_key_changes_at_height(BlockHeight height) const
  {
    std::scoped_lock lock(mutex_);
    CHECK_LE(height, current_height_);
    auto it = changes_by_height_.find(height);
    if (it == changes_by_height_.end()) {
      return KeyChangeBatch{};
    }
    return it->second;
  }

  void address_should_be_cached(ContractAddress address)
  {
    std::scoped_lock lock(mutex_);
    addresses_that_should_be_cached_.insert(address);
  }

  // RPC methods
  DBAuthResult authdb_get_auth_data(BlockHeight height,
                                    ContractAddress address);

  DBAuthResultBatch
  authdb_get_auth_data_batch(BlockHeight height,
                             ContractAddressBatch const &addresses);

  void set_intro_height(BlockHeight height) { intro_height_ = height; }
  size_t intro_count() const { return intro_count_; }

  x23::Async<DBAuthIntroResult> dbauth_intro(AuthCard card)
  {
    ++intro_count_;
    DBAuthIntroResult ret(DBAuthIntroOutcome::kIntroSuccess);
    ret.current_height() = intro_height_;
    co_return ret;
  }

private:
  // Find the most recent auth data for an address at or before the given height
  std::optional<AddressAuthData>
  find_auth_data_at_height(ContractAddress address, BlockHeight height) const;

  void check_should_be_cached(ContractAddress address) const
  {
    if (addresses_that_should_be_cached_.contains(address)) {
      throw std::runtime_error("Address should be cached but is not");
    }
  }

  std::unordered_map<ContractAddress, AddressAuthData, ByteArrayHasher>
      current_auth_state_;
  std::unordered_map<ContractAddress, KeyChange, ByteArrayHasher>
      current_key_changes_;

  std::unordered_set<ContractAddress, ByteArrayHasher>
      addresses_that_should_be_cached_;

  // Map from block height to the key changes made at that height
  std::map<BlockHeight, KeyChangeBatch> changes_by_height_;
  mutable ThreadCheckedMutex<std::mutex> mutex_;
  BlockHeight current_height_ = 0;

  // Read/written off the RPC thread, outside mutex_.
  std::atomic<BlockHeight> intro_height_ = -1;
  std::atomic<size_t> intro_count_ = 0;
};

static_assert(is_DBAuth1<MockDBAuthRpc *, x23::MaybeAsync>);

class MockDBAuthServer : public RpcServer<MockDBAuthRpc> {
public:
  MockDBAuthServer() : RpcServer(default_coropoll()) {}
};

} // namespace hogd
