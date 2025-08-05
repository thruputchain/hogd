#include "hogd/db_interface/db_coord_server.h"

#include <cstdio>
#include <cstdlib>
#include <utility>

#include "db_coord.hh"
#include "hogd/base/types.h"
#include "rpc_types.hh"
#include "x23/async.h"
#include "x23/cleanup.h"
#include "x23/event.h"
#include "x23/rpc.h"
#include "x23/sockio.h"

namespace hogd {

x23::Async<DbResult>
DbCoordServer::dbcoord_create_snapshot(BlockID parent_id, BlockID new_id,
                                       BlockExecutionMode mode)
{
  if (block_manager_->add_child(parent_id, new_id, mode)) {
    co_return DbResult::DbResultOK;
  }
  co_return DbResult::DbResultError;
}

x23::Async<DbBlockResult>
DbCoordServer::dbcoord_commit_snapshot(BlockID id)
{
  if (block_manager_->commit_block(id)) {
    //(geoff) TODO: Replace with actual BlockHash if available
    DbBlockResult result(DbResult::DbResultOK);
    // result.block_hash = ...; // Set block_hash if available
    co_return result;
  }
  DbBlockResult result(DbResult::DbResultError);
  co_return result;
}

x23::Async<DbBlockResult>
DbCoordServer::dbcoord_close_snapshot(BlockID id)
{
  auto blk = block_manager_->get_block_data(id);
  if (!blk) {
    DbBlockResult result(DbResult::DbResultError);
    co_return result;
  }
  blk->close_block();
  DbBlockResult result(DbResult::DbResultOK);
  // TODO(geoff): Set block_hash if available
  // result.block_hash = ...; // Set block_hash if available
  co_return result;
}

x23::Async<DbResult>
DbCoordServer::dbcoord_abandon_snapshot(BlockID id)
{
  // TODO(geoff): Implement actual abandon logic if needed
  // For now, just return ok for stub
  co_return DbResult::DbResultOK;
}

namespace {
template<typename Result, typename T>
ErrnoStatus<Result>
make_errno_status(T ret)
{
  // first check for error.
  if (ret == -1)
    return Errno{};

  // otherwise return the result type.
  assert(ret >= 0);
  if constexpr (std::is_integral_v<Result>) {
    return static_cast<Result>(ret);
  }
  else if (std::is_void_v<Result>) {
    assert(ret == 0);
    return {};
  }
  else {
    static_assert("unsupported result type");
  }
}

template<typename T> requires std::is_trivially_copyable_v<T>
inline ErrnoStatus<void>
sys_getsockname(int sockfd, T &val)
{
  socklen_t len = sizeof(val);
  return make_errno_status<void>(
      getsockname(sockfd, reinterpret_cast<sockaddr *>(&val), &len));
}

uint16_t
get_port(int fd)
{
  sockaddr_in saddr;
  // retrieve the port number if it was assigned automatically.
  ErrnoStatus<void> ret = sys_getsockname(fd, saddr);
  if (!ret)
    throw std::runtime_error("Failed to get socket name");
  return ntohs(saddr.sin_port);
}
} // namespace

void
DbCoordServer::start_server(uint32_t port_number)
{
  {
    auto lfd = x23::tcplisten(port_number);
    if (!lfd) {
      perror("listen");
      exit(EXIT_FAILURE);
    }
    std::lock_guard lock(mtx_);
    port_ = get_port(lfd.get());
    cv_.notify_all();

    [](x23::CoroPoll &cp, DbCoordServer *server,
       x23::UniqueFd lfd) -> x23::Detached {
      co_await cp.pool_.bg();
      while (auto c = co_await cp.accept(lfd)) {
        x23::rpc_stream_logged(cp, *c).server(server);
      }
    }(cp_, this, std::move(lfd));
  }
}

uint16_t
DbCoordServer::port()
{
  std::unique_lock lk(mtx_);
  cv_.wait(lk, [this] { return port_.has_value(); });
  return port_.value();
}

} // namespace hogd
