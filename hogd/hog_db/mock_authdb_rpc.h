#pragma once

#include <condition_variable>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>

#include "auth_rpc.hh"
#include "hogd/base/chunk_run_assembler.h"
#include "hogd/base/coropoll.h"
#include "hogd/base/rpc_server.h"
#include "hogd/base/xdr_util.h" // IWYU pragma: keep
#include "types.hh"
#include "x23/async.h"

namespace hogd {

// Mock implementation of AuthDB1 RPC interface that tracks received key changes
// for testing auth subscription functionality. A height's changes count as
// received once every chunk of its run is in; the run is assembled with the
// same framing checks the auth server applies.
class MockAuthDBRpc {
public:
  using rpc_service = AuthDB1;

  MockAuthDBRpc() = default;

  // The key changes of a height whose whole run has arrived, in chunk order.
  std::optional<KeyChangeBatch> get_changes_at_height(BlockHeight height) const
  {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = received_.find(height);
    if (it == received_.end()) {
      return std::nullopt;
    }
    return it->second;
  }

  // Wait for the whole run of key changes at the given block height
  void wait_for_changes(BlockHeight height);

  // RPC method
  void authdb_forward_key_changes_chunk(KeyChangeChunk chunk);

private:
  ChunkRunAssembler<BlockHeight, KeyChangeBatch> assembler_;
  std::map<BlockHeight, KeyChangeBatch> received_;
  mutable std::mutex mutex_;
  std::condition_variable cv_;
};

static_assert(is_AuthDB1<MockAuthDBRpc *, x23::MaybeAsync>);

class MockAuthDBServer : public RpcServer<MockAuthDBRpc> {
public:
  MockAuthDBServer() : RpcServer(default_coropoll()) {}
};

} // namespace hogd
