// rpc_from_sched.cc - handles RPC messages from the scheduler (also known as
// the mempool).

#include "hogd/hogdb/rpc_from_sched.h"

#include <cassert>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <netinet/in.h>
#include <stdexcept>
#include <sys/socket.h>
#include <utility>

// generated x23 headers
#include "db_sched.hh"

#include "rpc_types.hh"
#include "types.hh"

#include "hogd/base/log.h"
#include "hogd/base/status.h"
#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h" // IWYU pragma: keep
#include "hogd/hogdb/hog_types.h"
#include "hogd/hogdb/hogdb_dao.h"
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
DbSchedResult
to_db_result(HogDBStatus<T> ret)
{
  // TODO(abelay): the RPC error codes ought to be the same as or similar to
  // HogDBStatus; the current ones don't capture the range of possible errors.
  if (!ret) {
    error("to_db_result error: {}", ret.error());
    return DbSchedResult::DbSchedResultError;
  }
  return DbSchedResult::DbSchedResultOK;
}

} // namespace

x23::Async<HogAgeResult>
HogDBSchedServer::dbsched_get_hog_ages(const ContractAddressBatch &addresses)
{
  HogDBDAO &db = ctx_.get_db();
  HogAgeResult ret(DbSchedResult::DbSchedResultOK);
  ret.hog_age_batch().reserve(addresses.size());

  for (const ContractAddress &address : addresses) {
    HogDBStatus<HogAge> status = db.read_address_average_hog_age(address);
    if (!status) {
      co_return HogAgeResult(DbSchedResult::DbSchedResultError);
    }
    // TODO(jhumphri): Include the address in each index.
    ret.hog_age_batch().push_back(status.value());
  }
  co_return ret;
}

x23::Async<DbSchedResult>
HogDBSchedServer::dbsched_write_hog_ages(const HogAgeBatch &ages)
{
  HogDBDAO &db = ctx_.get_db();
  for (const HogAge &age : ages) {
    if (!db.write_address_average_hog_age(age)) {
      co_return DbSchedResult::DbSchedResultError;
    }
  }
  co_return DbSchedResult::DbSchedResultOK;
}

x23::Async<DbAccountAuthDataBatchResult>
HogDBSchedServer::dbsched_get_account_auth_data(BlockID block_id,
                                                ContractAddressBatch addresses)
{
  co_return DbAccountAuthDataBatchResult(DbSchedResult::DbSchedResultError);
}

namespace {

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
HogDBSchedServer::start_server(uint16_t port)
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

    [](x23::CoroPoll &cp, HogDBSchedServer *server,
       x23::UniqueFd lfd) -> x23::Detached {
      co_await cp.pool_.bg();
      while (auto c = co_await cp.accept(lfd)) {
        x23::rpc_stream_logged(cp, *c).server(server);
      }
    }(cp_, this, std::move(lfd));
  }
}

uint16_t
HogDBSchedServer::port()
{
  std::unique_lock lk(mtx_);
  cv_.wait(lk, [this] { return port_.has_value(); });
  // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
  return port_.value();
}

} // namespace hogd
