#include "hogd/test_ffi/db_context_wrapper.h"

#include <atomic>
#include <cstdlib>
#include <string>

#include "coord.hh"
#include "hogd/base/types.h"
#include "rpc.hh"
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

} // namespace hogd
