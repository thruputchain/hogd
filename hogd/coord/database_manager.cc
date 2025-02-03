#include "hogd/coord/database_manager.h"

namespace hogd {

void
DatabaseManager::add_database_shard(database_client_t &&client)
{
  database_clients_.emplace_back(std::move(client));
}

x23::Async<void>
DatabaseManager::open_block(BlockID parent_id, BlockID block_id)
{
  for (auto &client : database_clients_) {
    auto res = co_await client.open_database_snapshot(parent_id, block_id);
    if (res != RPCError::kNone) {
      throw std::runtime_error("failed to open block");
    }
  }
  co_return;
}

x23::Async<bool>
DatabaseManager::close_block(BlockID block_id)
{
  bool close_success = true;
  for (auto &client : database_clients_) {
    if (co_await client.close_snapshot(block_id) != RPCError::kNone) {
      close_success = false;
    }
  }
  co_return close_success;
}

x23::Async<void>
DatabaseManager::commit_block(BlockID block_id)
{
  for (auto &client : database_clients_) {
    co_await client.commit_snapshot(block_id);
  }
  co_return;
}

} // namespace hogd
