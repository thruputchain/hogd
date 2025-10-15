#include "hogd/hogexec/interface/executor.h"

#include <cassert>
#include <sys/socket.h>

#include "hogd/mempool/shared.h"
#include "hogd/version.h"
#include "sched_exec.hh"
#include "x23/cleanup.h"
#include "x23/refcnt.h"
#include "x23/rpc.h"
#include "x23/sockio.h"
#include "x23/srpc.h"

namespace hogd {

// static
IntroExecResult
MempoolExecutorClient::register_with_coordinator(
    x23::HostPort const &self_hostport,
    const sockaddr_storage &coordinator_sockaddr)
{
  auto client = create_client<SchedExec1>(coordinator_sockaddr, /*retry=*/true);
  const ExecCard card = {.protocol = version::kHogdProtocol,
                         .upid = {},
                         .address = self_hostport.show()};
  auto ret = client.schedexec_intro_exec(card);
  return ret;
}

} // namespace hogd
