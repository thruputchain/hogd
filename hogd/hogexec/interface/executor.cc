#include "hogd/hogexec/interface/executor.h"

#include <cassert>
#include <chrono>
#include <sys/socket.h>
#include <thread>

#include "hogd/base/log.h"
#include "hogd/version.h"
#include "sched_exec.hh"
#include "x23/cleanup.h"
#include "x23/refcnt.h"
#include "x23/rpc_common.h"
#include "x23/sockio.h"
#include "x23/srpc.h"

namespace hogd {

namespace {

SchedExec1::client<x23::RefPtr<x23::SRpcStream<x23::RpcLogger>>>
connect_to_coordinator(const sockaddr_storage &coordinator_sockaddr)
{
  while (true) {
    try {
      x23::UniqueFd fd = x23::tcpconnect(coordinator_sockaddr);
      CHECK(static_cast<bool>(fd));
      return x23::srpc_stream_logged(fd.release())->client(SchedExec1{});
    } catch (...) {
      error("Failed to connect to coordinator");
      // Wait briefly and then try again on the next iteration...
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
  }
}

} // namespace

// static
IntroExecResult
MempoolExecutorClient::register_with_coordinator(
    x23::HostPort const &self_hostport,
    const sockaddr_storage &coordinator_sockaddr)
{
  auto client = connect_to_coordinator(coordinator_sockaddr);
  const ExecCard card = {.protocol = version::kHogdProtocol,
                         .upid = {},
                         .address = self_hostport.show()};
  return client.schedexec_intro_exec(card);
}

} // namespace hogd
