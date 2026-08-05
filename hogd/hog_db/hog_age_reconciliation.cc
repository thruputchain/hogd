#include "hogd/hog_db/hog_age_reconciliation.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <mutex>
#include <span>
#include <utility>

#include "db_sched.hh"
#include "hogd/base/log.h"
#include "hogd/base/string.h"
#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h"
#include "hogd/container/concurrent_map.h"
#include "hogd/hog_db/hog_db_dao.h"
#include "hogd/hog_db/hog_types.h"
#include "hogd/hog_db/status.h"

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

HogDbStatus<void>
reconcile_address_hog_age_helper(const uint128_t balance, const HogAge &age,
                                 HogDbDAO &db)
{
  const uint128_t existing_total = sum_hogs(age);
  if (existing_total == balance) {
    return {};
  }

  HogAge new_age = age;
  const uint128_t to_schedule_before =
      from_xdr(age.num_hogs[kHogAgeGroupToSchedule]);
  const uint128_t already_scheduled_before =
      from_xdr(age.num_hogs[kHogAgeGroupAlreadyScheduled]);
  const uint128_t received_before =
      from_xdr(age.num_hogs[kHogAgeGroupReceived]);

  if (existing_total < balance) {
    const uint128_t deficit = balance - existing_total;
    new_age.num_hogs[kHogAgeGroupToSchedule] =
        to_xdr(to_schedule_before + deficit);
  }
  else {
    uint128_t excess = existing_total - balance;
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
  }

  return db.write_address_average_hog_age(new_age);
}

HogDbStatus<void>
reconcile_address_hog_age(HogDbDAO &db, const Address &address)
{
  HogDbStatus<int128_t> balance = db.read_address_balance(address);
  if (!balance.has_value()) {
    error("address 0x{} has a hog age row but could not read the balance row "
          "during reconciliation: {}",
          encode_hex(address.as_bytes()), balance.error());
    return balance.error();
  }
  if (*balance < 0) {
    error(
        "address 0x{} has a negative balance {} during hog age reconciliation",
        encode_hex(address.as_bytes()), *balance);
    return HogDbError(HogDbCode::kInternal);
  }
  HogDbStatus<HogAge> age = db.read_address_average_hog_age(address);
  if (!age.has_value()) {
    error("address 0x{} has a balance row but could not hog age row during "
          "reconciliation: {}",
          encode_hex(address.as_bytes()), age.error());
    return age.error();
  }

  return reconcile_address_hog_age_helper(*balance, *age, db);
}

} // namespace

HogDbStatus<void>
reconcile_startup_hog_ages(HogDbDAO &db)
{
  // [ConcurrentSet<T>] requires [T] to be an unsigned integral type, which
  // [Address] is not. [ConcurrentMap<K, V>] places no constraints on [K], and
  // its inserts are lock-free, so the scan below can run fully parallel: the
  // dedup is handled by the map rather than relying on a serial callback.
  // ([char] is an arbitrary placeholder value; we only use the keys.)
  ConcurrentMap<Address, char> addresses;
  db.get_ramdb().get_each_parallel(
      [&addresses](std::span<const std::byte> key, uint8_t /*type*/,
                   std::span<const std::byte> /*value*/) {
        if (is_prefixed_address_key(key, Prefix::kBalance) ||
            is_prefixed_address_key(key, Prefix::kHogAge)) {
          addresses.insert_or_get(address_from_prefixed_key(key));
        }
      });

  // Now reconcile.
  HogDbStatus<void> ret;
  std::mutex lock;
  addresses.for_each_parallel(
      [&ret, &lock, &db](std::pair<const Address &, char &> entry) {
        HogDbStatus<void> status = reconcile_address_hog_age(db, entry.first);
        if (!status.has_value()) {
          std::lock_guard<std::mutex> guard(lock);
          ret = status.error();
        }
      });

  return ret;
}

} // namespace hogd
