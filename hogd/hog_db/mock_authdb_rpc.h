#pragma once

#include <condition_variable>
#include <map>
#include <mutex>
#include <optional>

#include "auth_rpc.hh"
#include "hogd/base/coropoll.h"
#include "hogd/base/rpc_server.h"
#include "hogd/base/xdr_util.h" // IWYU pragma: keep
#include "types.hh"
#include "x23/async.h"

namespace hogd {

// Mock implementation of AuthDB1 RPC interface that tracks received key changes
// for testing auth subscription functionality.
class MockAuthDBRpc {
public:
  using rpc_service = AuthDB1;

  MockAuthDBRpc() = default;

  // Get key changes for a specific block height
  std::optional<KeyChangeBatch> get_changes_at_height(BlockHeight height) const
  {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = received_changes_.find(height);
    if (it == received_changes_.end()) {
      return std::nullopt;
    }
    return it->second;
  }

  // Wait for key changes to be received at the given block height
  void wait_for_changes(BlockHeight height);

  // RPC method
  void authdb_forward_key_changes(BlockHeight height,
                                  KeyChangeBatch key_changes);

private:
  std::map<BlockHeight, KeyChangeBatch> received_changes_;
  mutable std::mutex mutex_;
  std::condition_variable cv_;
};

static_assert(is_AuthDB1<MockAuthDBRpc *, x23::MaybeAsync>);

class MockAuthDBServer : public RpcServer<MockAuthDBRpc> {
public:
  MockAuthDBServer() : RpcServer(default_coropoll()) {}
};

} // namespace hogd
