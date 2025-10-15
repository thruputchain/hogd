#include "hogd/hogexec/interface/executor.h"

#include <cassert>
#include <sys/socket.h>

#include "hogd/mempool/shared.h"
#include "hogd/version.h"
#include "sched_exec.hh"
#include "x23/sockio.h"

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
  return client.schedexec_intro_exec(card)();
}

} // namespace hogd
