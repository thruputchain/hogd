#include "hogd/test_ffi/db_context_wrapper.h"

#include <atomic>
#include <cstdlib>
#include <memory>
#include <string>

#include "coord.hh"
#include "hogd/base/types.h"
#include "hogd/hogexec/runtime/storage_snapshot_cache.h"
#include "hogd/hogexec/runtime/syscall_entry.h"
#include "rpc.hh"
#include "rpc_types.hh"
#include "x23/rpc.h"

namespace hogd {

namespace {

struct PortAllocator {
  inline static std::atomic<uint32_t> port_start_ = 10000;

  static std::string allocate_port()
  {
    return std::to_string(port_start_.fetch_add(1));
  }
};

} // namespace

DBContext::DBContext()
  : db_to_exec_port(PortAllocator::allocate_port()),
    db_to_coord_port(PortAllocator::allocate_port()),
    exec_to_coord_port(PortAllocator::allocate_port()),
    db_(1, db_to_exec_port, db_to_coord_port), pool_(1), cp_(pool_),
    db_client_([this]() -> db_client_t {
      auto fd = cp_.tcpconnect("localhost", db_to_coord_port)();
      if (!fd) {
        exit(-1);
      }

      auto client_config = x23::rpc_stream_logged(cp_, *fd);
      return client_config.template client<DatabaseToCoordInterfaceV1>();
    }()),
    db_proxy_(cp_,
              DatabaseInfo{.database_address = "localhost",
                           .database_to_coord_port = {db_to_coord_port.c_str(),
                                                      db_to_coord_port.size()},
                           .database_to_exec_port = {db_to_exec_port.c_str(),
                                                     db_to_exec_port.size()}})
{
  db_.get_root_db().set_as_default_genesis();
}

void 
DBContext::advance_new_block()
{
if (current_block_id_ != 0) {
    if (db_client_.close_snapshot(current_block_id_)() != RPCError::kNone) {
    exit(-1);
    }
}
if (db_client_.open_database_snapshot(current_block_id_,
                                        current_block_id_ + 1)() !=
    RPCError::kNone) {
    exit(-1);
}
syscall_logfd = 1;
current_block_id_++;
snapshot_cache_ =
    std::make_unique<StorageSnapshotCache>(current_block_id_, db_proxy_);
}

} // namespace hogd
