#pragma once

#ifdef __linux__
#include <netinet/in.h>
#include <netinet/tcp.h>
#endif
#include <string_view>
#include <sys/socket.h>

namespace hogd {

struct ProposalCongestionReceipt {
  std::string_view before, after, status;
};

inline std::string_view
proposal_tcp_congestion(int fd) noexcept
{
#ifdef __linux__
  char algorithm[16]{};
  socklen_t length = sizeof(algorithm);
  if (getsockopt(fd, IPPROTO_TCP, TCP_CONGESTION, algorithm, &length) != 0 ||
      length > sizeof(algorithm))
    return "unavailable";
  auto value = std::string_view(algorithm, length);
  if (auto end = value.find('\0'); end != std::string_view::npos)
    value = value.substr(0, end);
  if (value == "cubic")
    return "cubic";
  if (value == "reno")
    return "reno";
#endif
  return "other";
}

inline ProposalCongestionReceipt
apply_proposal_congestion(int fd, std::string_view mode) noexcept
{
  const auto before = proposal_tcp_congestion(fd);
  if (before != "cubic")
    return {before, before, "stock-mismatch"};
  if (mode == "stock") {
    const auto after = proposal_tcp_congestion(fd);
    return {before, after, after == before ? "ok" : "readback-mismatch"};
  }
#ifdef __linux__
  if (mode == "reno") {
    constexpr char reno[] = "reno";
    if (setsockopt(fd, IPPROTO_TCP, TCP_CONGESTION, reno, sizeof(reno)) != 0)
      return {before, proposal_tcp_congestion(fd), "set-failed"};
    const auto after = proposal_tcp_congestion(fd);
    return {before, after, after == "reno" ? "ok" : "readback-mismatch"};
  }
#endif
  return {before, before, "invalid-mode"};
}

} // namespace hogd
