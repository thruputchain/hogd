#include "hogd/test_ffi/db_context_wrapper.h"

#include <cassert>
#include <cstdlib>
#include <memory>

#include "hogd/base/types.h"
#include "hogd/hogexec/runtime/database_proxy.h"
#include "hogd/hogexec/runtime/event_server_proxy.h"
#include "hogd/hogexec/runtime/storage_snapshot_cache.h"
#include "rpc_types.hh"
#include "x23/sockio.h"

namespace hogd {

DBContext::DBContext(uint32_t db_read_port)
  : block_manager_(current_block_id_, &db_),
    executor_server_(&block_manager_, db_read_port), db_proxy_(nullptr),
    snapshot_cache_(nullptr)
{
  db_proxy_ = std::make_unique<DatabaseProxy>(
      LocalMockDB{.read_port_number_ = executor_server_.port()});
  event_server_proxy_ = std::make_unique<EventServerProxy>(
      x23::HostPort{"localhost", event_server_exec_rpc_.start_server(0)});

  advance_new_block();
}

void
DBContext::advance_new_block(BlockExecutionMode mode)
{
  if (current_block_id_ > 0) {
    auto b = block_manager_.get_block_data(current_block_id_);
    assert(b);
    b->close_block();
  }

  [[maybe_unused]] auto res =
      block_manager_.add_child(current_block_id_, current_block_id_ + 1, mode);
  assert(res);
  current_block_id_++;
  snapshot_cache_ =
      std::make_unique<StorageSnapshotCache>(current_block_id_, *db_proxy_);
}

} // namespace hogd
