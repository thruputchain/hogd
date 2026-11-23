#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <string_view>

namespace hogd {

inline constexpr uint64_t kPaymentDbSampleStride = 256;
// The fixed sweep runs approximately 16 million transactions per account
// setting, with two measured RPCs per 256th transaction.
inline constexpr uint64_t kPaymentDbSampleLimit = 160'000;

inline bool
payment_db_env_matches(const char *key, std::string_view expected)
{
  const char *value = std::getenv(key);
  return value && value == expected;
}

inline bool
payment_db_queue_run(std::string_view host)
{
  if (!payment_db_env_matches("HOGD_CI_CONTAINER", "1") ||
      !payment_db_env_matches("HOGD_CI_OFFLINE", "1") ||
      !payment_db_env_matches("HOG_HOST", host))
    return false;
  const char *xid = std::getenv("HOG_XID");
  constexpr std::string_view prefix = "hogd-metrics-";
  if (!xid || !std::string_view(xid).starts_with(prefix))
    return false;
  const auto suffix = std::string_view(xid).substr(prefix.size());
  const auto split = suffix.find('-');
  const auto positive_decimal = [](std::string_view text) {
    return !text.empty() && text.front() != '0' &&
           std::ranges::all_of(text,
                               [](char c) { return c >= '0' && c <= '9'; });
  };
  return split != suffix.npos && positive_decimal(suffix.substr(0, split)) &&
         positive_decimal(suffix.substr(split + 1));
}

inline bool
payment_db_queue_enabled(std::string_view host)
{
  return payment_db_env_matches("GIT_BRANCH",
                                "codex/payment-srpc-queue-phases") &&
         payment_db_queue_run(host);
}

inline bool
payment_db_queue_pair(std::string_view host)
{
  return (payment_db_env_matches("GIT_BRANCH",
                                 "codex/payment-srpc-queue-phases") ||
          payment_db_env_matches("GIT_BRANCH",
                                 "codex/payment-srpc-queue-control")) &&
         payment_db_queue_run(host);
}

inline bool
payment_db_timing_enabled(std::string_view host)
{
  return payment_db_queue_pair(host) ||
         (payment_db_env_matches("GIT_BRANCH",
                                 "codex/payments-db-service-diagnostics") &&
          payment_db_env_matches("HOGD_CI_CONTAINER", "1") &&
          payment_db_env_matches("HOGD_CI_OFFLINE", "1") &&
          payment_db_env_matches("HOG_HOST", host));
}

} // namespace hogd
