#include "hogd/hogdb/hog_age_reconciliation.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <span>
#include <unordered_set>

#include "hogd/base/log.h"
#include "hogd/base/xdr_util.h"
#include "hogd/hogdb/hog_types.h"

namespace hogd {

namespace {

Address
address_from_prefixed_key(std::span<const std::byte> key)
{
  CHECK_EQ(key.size(), kAddressSize + 1);
  RawAddress raw{};
  std::ranges::copy(key.subspan(1),
                    std::begin(std::as_writable_bytes(std::span(raw))));
  return Address(raw);
}

bool
is_prefixed_address_key(std::span<const std::byte> key, Prefix prefix)
{
  return key.size() == kAddressSize + 1 &&
         static_cast<Prefix>(key.front()) == prefix;
}

uint128_t
sum_hogs(const HogAge &age)
{
  return from_xdr(age.num_hogs[kHogAgeGroupToSchedule]) +
         from_xdr(age.num_hogs[kHogAgeGroupAlreadyScheduled]) +
         from_xdr(age.num_hogs[kHogAgeGroupReceived]);
}

HogDBStatus<uint128_t>
get_owned_hogs(HogDBDAO &db, const Address &account)
{
  auto balance = db.read_address_balance(account);
  if (!balance.has_value()) {
    if (balance.error() == HogDBCode::kNotFound) {
      return uint128_t{0};
    }
    return balance.error();
  }
  if (*balance < 0) {
    error("negative balance for account {} during hog age reconciliation",
          account);
    return HogDBError(HogDBCode::kInternal);
  }
  return static_cast<uint128_t>(*balance);
}

HogDBStatus<void>
reconcile_account_hog_age(HogDBDAO &db, const Address &account)
{
  auto age = db.read_address_average_hog_age(account);
  if (!age.has_value()) {
    error("missing hog age entry for account {} during startup reconciliation",
          account);
    return age.error();
  }

  auto owned_hogs = get_owned_hogs(db, account);
  if (!owned_hogs.has_value()) {
    return owned_hogs.error();
  }

  const uint128_t existing_total = sum_hogs(*age);
  if (existing_total == *owned_hogs) {
    return {};
  }

  HogAge new_age = *age;
  const uint128_t to_schedule_before =
      from_xdr(age->num_hogs[kHogAgeGroupToSchedule]);
  const uint128_t already_scheduled_before =
      from_xdr(age->num_hogs[kHogAgeGroupAlreadyScheduled]);
  const uint128_t received_before = from_xdr(age->num_hogs[kHogAgeGroupReceived]);

  if (existing_total < *owned_hogs) {
    const uint128_t deficit = *owned_hogs - existing_total;
    new_age.num_hogs[kHogAgeGroupToSchedule] =
        to_xdr(to_schedule_before + deficit);
    info("reconciled hog age for {}: total {} -> {}, added {} hogs to "
         "ToSchedule",
         account, existing_total, *owned_hogs, deficit);
  }
  else {
    uint128_t excess = existing_total - *owned_hogs;
    uint128_t received = received_before;
    uint128_t already_scheduled = already_scheduled_before;
    uint128_t to_schedule = to_schedule_before;

    uint128_t deducted = std::min(excess, received);
    received -= deducted;
    excess -= deducted;

    deducted = std::min(excess, already_scheduled);
    already_scheduled -= deducted;
    excess -= deducted;

    deducted = std::min(excess, to_schedule);
    to_schedule -= deducted;
    excess -= deducted;

    CHECK_EQ(excess, 0UL);
    new_age.num_hogs[kHogAgeGroupReceived] = to_xdr(received);
    new_age.num_hogs[kHogAgeGroupAlreadyScheduled] = to_xdr(already_scheduled);
    new_age.num_hogs[kHogAgeGroupToSchedule] = to_xdr(to_schedule);

    info("reconciled hog age for {}: total {} -> {}, drained sequentially "
         "(Received {} -> {}, AlreadyScheduled {} -> {}, ToSchedule {} -> {})",
         account, existing_total, *owned_hogs, received_before, received,
         already_scheduled_before, already_scheduled, to_schedule_before,
         to_schedule);
  }

  return db.write_address_average_hog_age(new_age);
}

} // namespace

HogDBStatus<void>
reconcile_startup_hog_ages(HogDBDAO &db)
{
  std::unordered_set<Address> accounts;
  db.get_ramdb().get_each_parallel(
      [&accounts](std::span<const std::byte> key, uint8_t /*type*/,
                  std::span<const std::byte> /*value*/) {
        if (is_prefixed_address_key(key, Prefix::kBalance) ||
            is_prefixed_address_key(key, Prefix::kHogAge)) {
          accounts.insert(address_from_prefixed_key(key));
        }
      },
      1);

  for (const Address &account : accounts) {
    auto balance = db.read_address_balance(account);
    if (balance.has_value()) {
      auto age = db.read_address_average_hog_age(account);
      if (!age.has_value()) {
        error("account {} has a balance row but no hog age row", account);
        return age.error();
      }
    }
    else if (balance.error() != HogDBCode::kNotFound) {
      return balance.error();
    }

    auto status = reconcile_account_hog_age(db, account);
    if (!status.has_value()) {
      return status.error();
    }
  }

  return {};
}

} // namespace hogd
