#pragma once

#include <string>
#include <utility>
#include <vector>

#include "ci_server.hh"
#include "hogd/base/log.h"
#include "hogd/base/rpc_client.h"
#include "hogd/base/types.h"
#include "hogd/version.h"
#include "x23/rpc.h"
#include "x23/sockio.h"
#include "x23/srpc.h"
#include "x23/vec.h"

namespace hogd {

constexpr static uint16_t kControlServerPort = 11111;

class ControlClientWrapper {
public:
  ControlClientWrapper(
      CIServer1::client<x23::SRpcStream<x23::RpcLogger>::ClientRef> client)
    : client_(std::move(client))
  {}

  const x23::Buf kHogdGitHash = []() {
    auto str = std::string(version::kHogdGitHash);
    return x23::Buf{str.begin(), str.end()};
  }();

  // Asks the control server to start [program_name] and hands back whatever it
  // answered, without deciding what a failure means. [control_cli] reports a
  // failed start the way it reports every other command's failure -- a named
  // message and a nonzero exit -- rather than through the core dump
  // [start_server] produces.
  CIStartStatus try_start_server(std::string const &program_name,
                                 std::vector<std::string> const &program_args,
                                 std::string const &cpu_affinity = "")
  {
    ServerConfig config;
    config.program_name = x23::Buf{program_name.begin(), program_name.end()};
    for (auto const &arg : program_args) {
      config.program_args.push_back(x23::Buf{arg.begin(), arg.end()});
    }
    config.cpu_affinity = x23::Buf{cpu_affinity.begin(), cpu_affinity.end()};

    return client_.ciserver_start(kHogdGitHash, config);
  }

  CIStartStatus start_server(std::string const &program_name,
                             std::vector<std::string> const &program_args,
                             std::string const &cpu_affinity = "")
  {
    auto status = try_start_server(program_name, program_args, cpu_affinity);
    if (status.error_code() != CIServerErrorCode::kCIServerErrorCodeOK) {
      // Name the code the server sent. A start aimed at a control server left
      // over from an earlier run fails with
      // [kCIServerErrorCodeVersionMismatch], and a panic saying only "Failed
      // to start server" leaves the job log with nothing to tell that apart
      // from a mistyped program name -- which is how the core dump in hogd
      // issue #3945 reached the log with no cause attached.
      panic("Failed to start server {}: {}", program_name, status.error_code());
    }
    return status;
  }

  void stop_server() { client_.ciserver_stop(kHogdGitHash); }

  void shutdown_server() { client_.ciserver_shutdown(kHogdGitHash); }

  CIProcStatus query_proc_status()
  {
    return client_.ciserver_query_proc_status(kHogdGitHash);
  }

  CIWorkDirStatus query_work_dir()
  {
    return client_.ciserver_query_work_dir(kHogdGitHash);
  }

  CIServerErrorCode clear_work_dir()
  {
    return client_.ciserver_clear_work_dir(kHogdGitHash);
  }

private:
  CIServer1::client<x23::SRpcStream<x23::RpcLogger>::ClientRef> client_;
};

inline auto
connect_to_control_server(std::string const &host, std::string const &port,
                          bool retry = true)
{
  uint16_t port_num = std::stoi(port);
  info("connecting to control server on {}:{}", host, port);
  return ControlClientWrapper(
      make_srpc_client<CIServer1>(x23::HostPort{host, port_num}, retry));
}

} // namespace hogd
