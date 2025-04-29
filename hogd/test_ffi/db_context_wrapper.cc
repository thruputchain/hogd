#include "hogd/test_ffi/db_context_wrapper.h"

#include <cstdlib>
#include <memory>

#include "hogd/base/types.h"
#include "hogd/hogexec/runtime/database_proxy.h"
#include "hogd/hogexec/runtime/storage_snapshot_cache.h"

namespace hogd {

DBContext::DBContext(uint32_t db_read_port)
  : pool_(2), cp_(pool_), snapshot_server_(current_block_id_),
    db_proxy_(nullptr), snapshot_cache_(nullptr)
{
  snapshot_server_.start_server(db_read_port);
  db_proxy_ =
      std::make_unique<DatabaseProxy>(cp_, LocalMockDB{.read_port_number_ =
                                                           db_read_port});
  snapshot_cache_ =
      std::make_unique<StorageSnapshotCache>(current_block_id_, *db_proxy_);
}

void
DBContext::advance_new_block()
{
  current_block_id_++;
  snapshot_cache_ =
      std::make_unique<StorageSnapshotCache>(current_block_id_, *db_proxy_);
}

} // namespace hogd
