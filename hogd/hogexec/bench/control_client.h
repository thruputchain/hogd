#pragma once

#include <chrono>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "ci_server.hh"
#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "hogd/version.h"
#include "x23/event.h"
#include "x23/rpc.h"
#include "x23/vec.h"

namespace hogd {

constexpr static uint16_t kControlServerPort = 11111;

class ControlClientWrapper {
public:
  ControlClientWrapper(
      CIServer1::client<x23::RpcStream<x23::RpcLogger>::ClientRef> client)
    : client_(std::move(client))
  {}

  void start_server(std::string const &program_name,
                    std::vector<std::string> const &program_args)
  {
    ServerConfig config;
    config.program_name = x23::BoundedStr{program_name.c_str()};
    for (auto const &arg : program_args) {
      config.program_args.push_back(x23::BoundedStr{arg.c_str()});
    }

    if (client_.ciserver_start(version::kHogdGitHash, config)() !=
        CIServerErrorCode::kCIServerErrorCodeOK) {
      throw std::runtime_error("Failed to start server");
    }
  }

  void stop_server() { client_.ciserver_stop(version::kHogdGitHash)(); }

  void shutdown_server() { client_.ciserver_shutdown(version::kHogdGitHash)(); }

  CIProcStatus query_proc_status()
  {
    return client_.ciserver_query_proc_status(version::kHogdGitHash)();
  }

private:
  CIServer1::client<x23::RpcStream<x23::RpcLogger>::ClientRef> client_;
};

inline auto
connect_to_control_server(
    x23::CoroPoll &cp, std::string const &host, std::string const &port,
    std::chrono::milliseconds timeout = std::chrono::milliseconds(0))
{
  auto start_time = std::chrono::steady_clock::now();
  while (true) {
    info("connecting to control server on {}:{}", host, port);
    auto fd = cp.tcpconnect(host, port)();
    if (!fd) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      auto elapsed = std::chrono::steady_clock::now() - start_time;
      if (timeout != std::chrono::milliseconds(0) && elapsed > timeout) {
        throw std::runtime_error("timeout connecting to control server");
      }
      continue;
    }
    auto client_config = x23::rpc_stream_logged(cp, *fd);
    return ControlClientWrapper(client_config->template client<CIServer1>());
  }
}

} // namespace hogd
