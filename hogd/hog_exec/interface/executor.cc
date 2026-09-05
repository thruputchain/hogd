#include "hogd/hog_exec/interface/executor.h"

#include <cassert>
#include <sys/socket.h>

#include "hogd/base/rpc_client.h"
#include "hogd/version.h"
#include "sched_exec.hh"
#include "x23/refcnt.h"
#include "x23/rpc_common.h"
#include "x23/sockio.h"
#include "x23/srpc.h"

namespace hogd {

namespace {

SchedExec1::client<x23::RefPtr<x23::SRpcStream<x23::RpcLogger>>>
connect_to_coordinator(const sockaddr_storage &coordinator_sockaddr)
{
  return make_srpc_client<SchedExec1>(coordinator_sockaddr, true);
}

} // namespace

// static
IntroExecResult
MempoolExecutorClient::register_with_coordinator(
    x23::HostPort const &self_hostport,
    const sockaddr_storage &coordinator_sockaddr)
{
  auto client = connect_to_coordinator(coordinator_sockaddr);
  const ExecCard card = {.protocol = version::kHogdProtocol(),
                         .upid = {},
                         .address = self_hostport.show()};
  return client.schedexec_intro_exec(card);
}

} // namespace hogd
