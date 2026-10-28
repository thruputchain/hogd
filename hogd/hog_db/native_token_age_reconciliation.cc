#include "hogd/hog_db/native_token_age_reconciliation.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <limits>
#include <mutex>
#include <optional>
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

// Returns std::nullopt if the three group counts do not fit in a uint128_t.
//
// Neither durable-state check upstream of here catches this. The RAMDB log
// CRC covers accidental corruption but is unkeyed, so it is recomputed by any
// deliberate rewrite; and the consensus db_hash does not cover the row at all,
// because Prefix::kNativeTokenAge returns false from is_state_item(). A
// tampered native-token age row therefore reaches reconciliation with both
// checks satisfied.
//
// If the wrapped total happens to equal the balance, reconciliation below
// would then accept the row as already reconciled and leave it in place. The
// corruption would only surface much later, on an ordinary transfer into the
// account, as a fatal denominator check inside the mempool scheduler's
// compute_average_age() -- a different process, arbitrarily later, with
// nothing pointing back at the row. Detect it here instead, at startup, where
// it can be reported against the address that carries it (#3683).
std::optional<uint128_t>
sum_native_tokens(const NativeTokenAge &age)
{
  constexpr uint128_t kMax = std::numeric_limits<uint128_t>::max();
  const uint128_t to_schedule =
      from_xdr(age.num_native_tokens[kNativeTokenAgeGroupToSchedule]);
  const uint128_t already_scheduled =
      from_xdr(age.num_native_tokens[kNativeTokenAgeGroupAlreadyScheduled]);
  const uint128_t received =
      from_xdr(age.num_native_tokens[kNativeTokenAgeGroupReceived]);

  if (already_scheduled > kMax - to_schedule) {
    return std::nullopt;
  }
  const uint128_t partial = to_schedule + already_scheduled;
  if (received > kMax - partial) {
    return std::nullopt;
  }
  return partial + received;
}

// [address] is the address the row was keyed and read under. Diagnostics must
// use it rather than the embedded age.address: the two can disagree on a
// tampered row while the XDR stays valid, and
// read_address_average_native_token_age() does not cross-check them, so
// trusting the payload could send an operator to repair an address that is not
// the corrupt one.
HogDbStatus<void>
reconcile_address_native_token_age_helper(const Address &address,
                                          const uint128_t balance,
                                          const NativeTokenAge &age,
                                          HogDbDAO &db)
{
  const std::optional<uint128_t> summed = sum_native_tokens(age);
  if (!summed.has_value()) {
    error("address 0x{} has a corrupt native-token age row: group counts {}, "
          "{}, {} "
          "overflow uint128 when summed. Refusing to reconcile; the durable "
          "native-token age state for this address must be repaired or the "
          "volume "
          "replaced.",
          encode_hex(address.as_bytes()),
          from_xdr(age.num_native_tokens[kNativeTokenAgeGroupToSchedule]),
          from_xdr(age.num_native_tokens[kNativeTokenAgeGroupAlreadyScheduled]),
          from_xdr(age.num_native_tokens[kNativeTokenAgeGroupReceived]));
    return HogDbError(HogDbCode::kInternal);
  }
  // average_ages are block heights since genesis, but BlockHeight is a signed
  // hyper, so a tampered row can carry a negative average that XDR and the
  // count checks above accept. Left in place, the mempool scheduler copies the
  // signed to-schedule average straight into its ascending ready-account key
  // (get_account_key), where a negative value sorts ahead of every valid
  // nonnegative age and silently steals scheduling priority. Reject it here,
  // at startup, against the address that carries it, rather than letting the
  // corruption surface later in a different process (#3934).
  for (size_t group = 0; group < kNativeTokenAgeNumGroups; ++group) {
    if (age.average_ages[group] < 0) {
      error("address 0x{} has a corrupt native-token age row: average ages {}, "
            "{}, {} "
            "contain a negative value. Refusing to reconcile; the durable "
            "native-token age state for this address must be repaired or the "
            "volume replaced.",
            encode_hex(address.as_bytes()),
            age.average_ages[kNativeTokenAgeGroupToSchedule],
            age.average_ages[kNativeTokenAgeGroupAlreadyScheduled],
            age.average_ages[kNativeTokenAgeGroupReceived]);
      return HogDbError(HogDbCode::kInternal);
    }
  }

  const uint128_t existing_total = *summed;
  if (existing_total == balance) {
    return {};
  }

  NativeTokenAge new_age = age;
  const uint128_t to_schedule_before =
      from_xdr(age.num_native_tokens[kNativeTokenAgeGroupToSchedule]);
  const uint128_t already_scheduled_before =
      from_xdr(age.num_native_tokens[kNativeTokenAgeGroupAlreadyScheduled]);
  const uint128_t received_before =
      from_xdr(age.num_native_tokens[kNativeTokenAgeGroupReceived]);

  if (existing_total < balance) {
    const uint128_t deficit = balance - existing_total;
    new_age.num_native_tokens[kNativeTokenAgeGroupToSchedule] =
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
    new_age.num_native_tokens[kNativeTokenAgeGroupReceived] = to_xdr(received);
    new_age.num_native_tokens[kNativeTokenAgeGroupAlreadyScheduled] =
        to_xdr(already_scheduled);
    new_age.num_native_tokens[kNativeTokenAgeGroupToSchedule] =
        to_xdr(to_schedule);
  }

  return db.write_address_average_native_token_age(new_age);
}

HogDbStatus<void>
reconcile_address_native_token_age(HogDbDAO &db, const Address &address)
{
  HogDbStatus<int128_t> balance = db.read_address_balance(address);
  if (!balance.has_value()) {
    error("address 0x{} has a native-token age row but could not read the "
          "balance row "
          "during reconciliation: {}",
          encode_hex(address.as_bytes()), balance.error());
    return balance.error();
  }
  if (*balance < 0) {
    error("address 0x{} has a negative balance {} during native-token age "
          "reconciliation",
          encode_hex(address.as_bytes()), *balance);
    return HogDbError(HogDbCode::kInternal);
  }
  HogDbStatus<NativeTokenAge> age =
      db.read_address_average_native_token_age(address);
  if (!age.has_value()) {
    error("address 0x{} has a balance row but could not native-token age row "
          "during "
          "reconciliation: {}",
          encode_hex(address.as_bytes()), age.error());
    return age.error();
  }

  return reconcile_address_native_token_age_helper(address, *balance, *age, db);
}

} // namespace

HogDbStatus<void>
reconcile_startup_native_token_ages(HogDbDAO &db)
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
            is_prefixed_address_key(key, Prefix::kNativeTokenAge)) {
          addresses.insert_or_get(address_from_prefixed_key(key));
        }
      });

  // Now reconcile.
  HogDbStatus<void> ret;
  std::mutex lock;
  addresses.for_each_parallel(
      [&ret, &lock, &db](std::pair<const Address &, char &> entry) {
        HogDbStatus<void> status =
            reconcile_address_native_token_age(db, entry.first);
        if (!status.has_value()) {
          std::lock_guard<std::mutex> guard(lock);
          ret = status.error();
        }
      });

  return ret;
}

} // namespace hogd
