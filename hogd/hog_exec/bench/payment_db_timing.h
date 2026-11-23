#pragma once

#include <cstdint>
#include <cstdlib>
#include <string_view>

namespace hogd {

inline constexpr uint64_t kPaymentDbSampleStride = 256;
// The fixed sweep runs approximately 16 million transactions per account
// setting, with two measured RPCs per 256th transaction.
inline constexpr uint64_t kPaymentDbSampleLimit = 160'000;

// Shared by the driver and the typed DB process launch arguments.
inline bool
payment_db_affinity_arm(std::string_view arm)
{
  return arm == "llc-a-1" || arm == "llc-b-1" || arm == "split-1" ||
         arm == "split-2" || arm == "stock";
}

inline bool
payment_db_timing_enabled(std::string_view host)
{
  const auto matches = [](const char *name, std::string_view expected) {
    const char *value = std::getenv(name);
    return value && value == expected;
  };
  return matches("GIT_BRANCH", "codex/payments-db-service-diagnostics") &&
         matches("HOGD_CI_CONTAINER", "1") && matches("HOGD_CI_OFFLINE", "1") &&
         matches("HOG_HOST", host);
}

} // namespace hogd
