#include "hogd/test_ffi/test_bindings.h"

#include <cstdlib>
#include <memory>
#include <string>

#include "coord.hh"
#include "hogd/hogdb/hogdb_core.h"
#include "hogd/hogexec/hogexec_core.h"
#include "rpc.hh"
#include "rpc_types.hh"
#include "x23/event.h"
#include "x23/rpc.h"
#include "x23/threadpool.h"

namespace hogd {

namespace {
const std::string db_to_exec_port = std::to_string(kDBToExecutorPortNum);
const std::string db_to_coord_port = std::to_string(kDBToCoordPortNum);
const std::string exec_to_coord_port = std::to_string(kExecToCoordPortNum);

class DBContext {
public:
    using db_client_t = DatabaseToCoordInterfaceV1::client<
        x23::RpcStream<x23::RpcLogger>::ClientRef>;

    DBContext()
        : db_(1, db_to_exec_port, db_to_coord_port)
        , pool_(1)
        , cp_(pool_)
        , db_client_([this] () -> db_client_t {
                auto fd = cp_.tcpconnect("localhost", db_to_coord_port)();
                if (!fd) {
                    exit(-1);
                }

                auto client_config = x23::rpc_stream_logged(cp_, *fd);
                return client_config.template client<DatabaseToCoordInterfaceV1>();
    }()) {}

    void advance_new_block() {
        if (current_block_id_ != 0) {
            if (db_client_.close_snapshot(current_block_id_)() != RPCError::kNone) {
                exit(-1);
            }
        }
        if (db_client_.open_database_snapshot(current_block_id_, current_block_id_ + 1)() != RPCError::kNone) {
            exit(-1);
        }
        current_block_id_ ++;
    }

private:
    HogdbCore db_;
    x23::ThreadPool pool_;
    x23::CoroPoll cp_;
    // assumes genesis is 0
    BlockID current_block_id_ = 0;

    db_client_t db_client_;
};

DBContext *
make_new_db()
{
  return new DBContext();
}
void
free_db(DBContext *ptr)
{
  delete ptr;
}
} // namespace

} // namespace hogd

void *
create_db()
{
  return hogd::make_new_db();
}
void
free_db(void *ptr)
{
  hogd::free_db(static_cast<hogd::DBContext *>(ptr));
}

void* open_syscall_handler(void *db) {

    return nullptr;
}
