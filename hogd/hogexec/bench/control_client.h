#pragma once

#include <stdexcept>
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

  void start_server(std::string const &program_name,
                    std::vector<std::string> const &program_args,
                    std::string const &cpu_affinity = "")
  {
    ServerConfig config;
    config.program_name = x23::Buf{program_name.begin(), program_name.end()};
    for (auto const &arg : program_args) {
      config.program_args.push_back(x23::Buf{arg.begin(), arg.end()});
    }
    config.cpu_affinity = x23::Buf{cpu_affinity.begin(), cpu_affinity.end()};

    auto err = client_.ciserver_start(kHogdGitHash, config);
    if (err != CIServerErrorCode::kCIServerErrorCodeOK) {
      throw std::runtime_error("Failed to start server due to error {}", err);
    }
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
