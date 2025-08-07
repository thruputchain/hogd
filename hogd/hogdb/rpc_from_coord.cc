// rpc_from_coord.cc - handles RPC messages from the coordinator.

#include "hogd/hogdb/rpc_from_coord.h"

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <netinet/in.h>
#include <stdexcept>
#include <sys/socket.h>
#include <utility>

// generated x23 headers
#include "db_coord.hh"
#include "hogd/base/status.h"
#include "rpc_types.hh"

#include "hogd/base/types.h"
#include "hogd/hogdb/block_tx.h"
#include "hogd/hogdb/rpc_context.h"
#include "hogd/hogdb/status.h"
#include "x23/async.h"
#include "x23/cleanup.h"
#include "x23/event.h"
#include "x23/rpc.h"
#include "x23/sockio.h"

namespace hogd {

namespace {

template<typename T>
DbResult
to_db_result(HogDBStatus<T> ret)
{
  // TODO(abelay): the RPC error codes ought to be the same as or similar to
  // HogDBStatus; the current ones don't capture the range of possible errors.
  if (!ret)
    return DbResult::DbResultError;
  return DbResult::DbResultOK;
}

} // namespace

x23::Async<DbResult>
HogDBCoordServer::dbcoord_create_snapshot(BlockID parent_id, BlockID new_id,
                                          BlockExecutionMode mode)
{
  // TODO(abelay): the parent_id must be last committed snapshot. We could
  // check that here if we had the block height rather than the ID.
  co_return to_db_result(ctx_.open_block_tx(new_id));
}

x23::Async<DbBlockResult>
HogDBCoordServer::dbcoord_commit_snapshot(BlockID id)
{
  auto *ret = ctx_.get_block_tx(id);
  if (!ret) {
    co_return DbBlockResult(DbResult::DbResultError);
  }

  BlockTX &btx = *ret;
  co_return DbBlockResult(to_db_result(btx.commit()));
}

x23::Async<DbResult>
HogDBCoordServer::dbcoord_abandon_snapshot(BlockID id)
{
  HogDBStatus<BlockTX *> ret = ctx_.open_block_tx(id);
  if (!ret)
    co_return to_db_result(ret);

  BlockTX &btx = **ret;
  btx.abort();
  co_return DbResult::DbResultOK;
}

x23::Async<DbBlockResult>
HogDBCoordServer::dbcoord_close_snapshot(BlockID id) // NOLINT
{
  // TODO(abelay): Get rid of this. This should not be a separate RPC method.
  // Instead both commit and abort should implicitly close.

  // ctx_.close_block_tx(id);
  co_return DbBlockResult(DbResult::DbResultOK);
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
  socklen_t len = sizeof(val); // NOLINT(misc-include-cleaner)
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
HogDBCoordServer::start_server(uint16_t port)
{
  {
    auto lfd = x23::tcplisten(port);
    if (!lfd) {
      perror("listen");
      exit(EXIT_FAILURE);
    }

    std::lock_guard lock(mtx_);
    port_ = get_port(lfd.get());
    cv_.notify_all();

    [](x23::CoroPoll &cp, HogDBCoordServer *server,
       x23::UniqueFd lfd) -> x23::Detached {
      co_await cp.pool_.bg();
      while (auto c = co_await cp.accept(lfd)) {
        x23::rpc_stream_logged(cp, *c).server(server);
      }
    }(cp_, this, std::move(lfd));
  }
}

uint16_t
HogDBCoordServer::port()
{
  std::unique_lock lk(mtx_);
  cv_.wait(lk, [this] { return port_.has_value(); });
  // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
  return port_.value();
}

} // namespace hogd
