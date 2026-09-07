#pragma once

#include <chrono>
#include <expected>
#include <format>
#include <optional>
#include <sys/socket.h>
#include <system_error>
#include <thread>

#include "hogd/base/coropoll.h"
#include "hogd/base/log.h"
#include "hogd/base/status.h"
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
using rpc_client_t = typename RpcService::template client<
    x23::RpcStream<x23::RpcLogger>::ClientRef>;

// Passed as [retry_timeout] to keep retrying for as long as it takes. Only
// appropriate when the peer is guaranteed to come up eventually; a peer that
// can die during startup should be given a real bound, so its death surfaces
// as a failure here rather than as a hang for whatever timeout is furthest
// out.
constexpr auto kRetryForever = std::chrono::milliseconds::zero();

template<typename RpcService>
x23::Async<hogd::ErrnoStatus<rpc_client_t<RpcService>>>
// This function creates a coroutine, so be careful to only pass parameters by
// reference if you know their lifetime will outlast the coroutine.
make_rpc_client(sockaddr_storage ss, x23::CoroPoll &cp, bool retry = false,
                size_t max_message_size = x23::default_max_message_size,
                std::chrono::milliseconds retry_timeout = kRetryForever)
{
  info("make_rpc_client: connecting to {}", x23::HostPort(ss));
  std::optional<std::chrono::steady_clock::time_point> retry_deadline;
  if (retry_timeout > kRetryForever) {
    retry_deadline = std::chrono::steady_clock::now() + retry_timeout;
  }
  while (true) {
    std::expected<int, x23::Errno> res = co_await cp.tcpconnect(&ss);
    if (!res) {
      if (retry && res.error().error_code() == std::errc::connection_refused) {
        if (retry_deadline.has_value() &&
            std::chrono::steady_clock::now() >= *retry_deadline) {
          error("RPC client on {} was still refusing connections after {}ms; "
                "giving up",
                x23::show_ipaddr(ss), retry_timeout.count());
          co_return std::unexpected(res.error());
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        warn_every_n_sec(5.0F, "RPC client is not ready");
        continue;
      }
      error("could not create RPC client on {}: {}", x23::show_ipaddr(ss),
            res.error().error_code().message());
      co_return std::unexpected(res.error());
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
                size_t max_message_size = x23::default_max_message_size,
                std::chrono::milliseconds retry_timeout = kRetryForever)
{
  return make_rpc_client<RpcService>(ss, default_coropoll(), retry,
                                     max_message_size, retry_timeout);
}

template<typename RpcService>
auto
make_rpc_client(x23::HostPort host_port, x23::CoroPoll &cp, bool retry = false,
                size_t max_message_size = x23::default_max_message_size,
                std::chrono::milliseconds retry_timeout = kRetryForever)
{
  auto ss = host_port.to_addr(true);
  if (!ss) {
    panic("failed to convert host port to sockaddr_storage: {}", ss.error());
  }
  return make_rpc_client<RpcService>(*ss, cp, retry, max_message_size,
                                     retry_timeout);
}

template<typename RpcService>
auto
make_rpc_client(x23::HostPort host_port, bool retry = false,
                size_t max_message_size = x23::default_max_message_size,
                std::chrono::milliseconds retry_timeout = kRetryForever)
{
  auto ss = host_port.to_addr(true);
  if (!ss) {
    panic("failed to convert host port to sockaddr_storage: {}", ss.error());
  }
  return make_rpc_client<RpcService>(*ss, default_coropoll(), retry,
                                     max_message_size, retry_timeout);
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
