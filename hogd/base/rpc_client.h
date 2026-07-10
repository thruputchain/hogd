#pragma once

#include <chrono>
#include <expected>
#include <format>
#include <stdexcept>
#include <sys/socket.h>
#include <system_error>
#include <thread>

#include "hogd/base/coropoll.h"
#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "x23/async.h"
#include "x23/cleanup.h"
#include "x23/cpumap.h"
#include "x23/err.h"
#include "x23/event.h"
#include "x23/rpc.h"
#include "x23/sockio.h"
#include "x23/srpc.h"

namespace hogd {

template<typename RpcService>
x23::Async<typename RpcService::template client<
    x23::RpcStream<x23::RpcLogger>::ClientRef>>
// This function creates a coroutine, so be careful to only pass parameters by
// reference if you know their lifetime will outlast the coroutine.
make_rpc_client(sockaddr_storage ss, x23::CoroPoll &cp, bool retry = false,
                size_t max_message_size = x23::default_max_message_size)
{
  info("make_rpc_client: connecting to {}", x23::HostPort(ss));
  while (true) {
    std::expected<int, x23::Errno> res = co_await cp.tcpconnect(&ss);
    if (!res) {
      if (retry && res.error().error_code() == std::errc::connection_refused) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        warn_every_n_sec(5.0F, "RPC client is not ready");
        continue;
      }
      error("could not create RPC client on {}: {}", x23::show_ipaddr(ss),
            res.error().error_code().message());
      throw std::runtime_error(
          std::format("could not create RPC client on {}: {}",
                      x23::show_ipaddr(ss),
                      res.error().error_code().message()));
    }
    x23::UniqueFd fd = x23::UniqueFd(*res);

    auto client_config = x23::rpc_stream_logged(cp, fd.release());
    client_config->max_message_size = max_message_size;
    co_return client_config->template client<RpcService>();
  }
}

template<typename RpcService>
auto
make_rpc_client(sockaddr_storage &ss, bool retry = false,
                size_t max_message_size = x23::default_max_message_size)
{
  return make_rpc_client<RpcService>(ss, default_coropoll(), retry,
                                     max_message_size);
}

template<typename RpcService>
auto
make_rpc_client(x23::HostPort host_port, x23::CoroPoll &cp, bool retry = false,
                size_t max_message_size = x23::default_max_message_size)
{
  auto ss = host_port.to_addr(true);
  if (!ss) {
    panic("failed to convert host port to sockaddr_storage: {}", ss.error());
  }
  return make_rpc_client<RpcService>(*ss, cp, retry, max_message_size);
}

template<typename RpcService>
auto
make_rpc_client(x23::HostPort host_port, bool retry = false,
                size_t max_message_size = x23::default_max_message_size)
{
  auto ss = host_port.to_addr(true);
  if (!ss) {
    panic("failed to convert host port to sockaddr_storage: {}", ss.error());
  }
  return make_rpc_client<RpcService>(*ss, default_coropoll(), retry,
                                     max_message_size);
}

template<typename RpcService>
auto
make_srpc_client(sockaddr_storage &ss, bool retry = false,
                 size_t max_message_size = x23::default_max_message_size)
{
  while (true) {
    x23::UniqueFd fd;
    try {
      fd = x23::tcpconnect(ss);
    } catch (const std::system_error &e) {
      if (retry && e.code() == std::errc::connection_refused) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        warn_every_n_sec(5.0F, "RPC client on {} is not ready",
                         x23::show_ipaddr(ss));
        continue;
      }
      error("caught system error: {} {}", e.what(), e.code().message());
      throw;
    }

    auto client_config = x23::srpc_stream_logged(fd.release());
    client_config->max_message_size_ = max_message_size;
    return client_config->template client<RpcService>();
  }
}

template<typename RpcService>
auto
make_srpc_client(x23::HostPort host_port, bool retry = false,
                 size_t max_message_size = x23::default_max_message_size)
{
  auto ss = host_port.to_addr(true);
  if (!ss) {
    panic("failed to convert host port to sockaddr_storage: {}", ss.error());
  }
  return make_srpc_client<RpcService>(*ss, retry, max_message_size);
}

// TODO(geoff): setting max_message_size isn't implemented for the srpc
// multiclient yet
template<typename RpcService>
auto
make_srpc_multiclient(sockaddr_storage &ss, bool retry = false,
                      x23::CpuMap const &map = *x23::llc_map)
{
  while (true) {
    try {
      auto client = x23::SRpcMulticlient<x23::RpcLogger>::make(ss, map);
      return client->template client<RpcService>();
    } catch (const std::system_error &e) {
      if (retry && e.code() == std::errc::connection_refused) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        warn_every_n_sec(5.0F, "RPC client on {} is not ready",
                         x23::show_ipaddr(ss));
        continue;
      }
      error("caught system error: {} {}", e.what(), e.code().message());
      throw;
    }
  }
}

template<typename RpcService>
auto
make_srpc_multiclient(x23::HostPort host_port, bool retry = false,
                      x23::CpuMap const &map = *x23::llc_map)
{
  auto ss = host_port.to_addr(true);
  if (!ss) {
    panic("failed to convert host port to sockaddr_storage: {}", ss.error());
  }
  return make_srpc_multiclient<RpcService>(*ss, retry, map);
}

template<typename RpcService>
auto
make_rpc_multiclient(sockaddr_storage &ss, x23::CoroPoll &cp,
                     bool retry = false, x23::CpuMap const &map = *x23::llc_map,
                     size_t max_message_size = x23::default_max_message_size)
{
  while (true) {
    try {
      auto client = x23::RpcMulticlient<x23::RpcLogger>::make(
          cp, ss, map, x23::RpcLogger::default_logfd, max_message_size);
      return client->template client<RpcService>();
    } catch (const std::system_error &e) {
      if (retry && e.code() == std::errc::connection_refused) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        warn_every_n_sec(5.0F, "RPC client on {} is not ready",
                         x23::show_ipaddr(ss));
        continue;
      }
      error("caught system error: {} {}", e.what(), e.code().message());
      throw;
    }
  }
}

template<typename RpcService>
auto
make_rpc_multiclient(x23::HostPort host_port, bool retry = false,
                     x23::CpuMap const &map = *x23::llc_map,
                     size_t max_message_size = x23::default_max_message_size)
{
  auto ss = host_port.to_addr(true);
  if (!ss) {
    panic("failed to convert host port to sockaddr_storage: {}", ss.error());
  }
  return make_rpc_multiclient<RpcService>(*ss, default_coropoll(), retry, map,
                                          max_message_size);
}

} // namespace hogd
