#include "hogd/hog_db/shard_identity.h"

#include <cstdint>
#include <expected>
#include <format>
#include <optional>
#include <string>

#include "sched_db.hh" // ShardIdentity

#include "hogd/db_shard_routing/sharding.h" // kShardFunctionId
#include "hogd/hog_db/hog_db_dao.h"
#include "hogd/hog_db/status.h"

namespace hogd {

std::expected<ShardIdentity, std::string>
establish_shard_identity(HogDbDAO &db,
                         std::optional<std::uint32_t> flag_shard_id,
                         std::optional<std::uint32_t> flag_total_shards)
{
  // Stored under a kGlobalMetadata key, so it is excluded from the consensus
  // hash.
  HogDbStatus<ShardIdentity> persisted = db.read_shard_identity();
  if (persisted) {
    ShardIdentity identity = *persisted;
    if ((flag_shard_id && *flag_shard_id != identity.shard_id) ||
        (flag_total_shards && *flag_total_shards != identity.total_shards)) {
      return std::unexpected(std::format(
          "shard identity mismatch: persisted {{id={}, total={}}} but flags "
          "{{id={}, total={}}}",
          identity.shard_id, identity.total_shards,
          flag_shard_id ? std::to_string(*flag_shard_id) : "unset",
          flag_total_shards ? std::to_string(*flag_total_shards) : "unset"));
    }
    if (identity.shard_function_id != kShardFunctionId) {
      return std::unexpected(
          std::format("shard routing-function mismatch: persisted {} but this "
                      "binary uses {}",
                      identity.shard_function_id, kShardFunctionId));
    }
    return identity;
  }
  if (persisted.error().code() != HogDbCode::kNotFound) {
    // An unreadable identity record is not the same as "no record" -- do not
    // silently overwrite it.
    return std::unexpected(
        std::format("failed to read shard identity: {}", persisted.error()));
  }

  ShardIdentity identity{.shard_id = flag_shard_id.value_or(0),
                         .total_shards = flag_total_shards.value_or(1),
                         .shard_function_id = kShardFunctionId};
  // A db with committed state but no identity predates sharding, so it holds
  // the full single-shard dataset and can only be shard 0 of 1 -- reject flags
  // that would relabel it. A fresh db may adopt any valid identity.
  if (db.read_dbtip().has_value() &&
      (identity.shard_id != 0 || identity.total_shards != 1)) {
    return std::unexpected(std::format(
        "db has committed state but no shard identity (pre-sharding); it can "
        "only be shard 0 of 1, not id={} of {}",
        identity.shard_id, identity.total_shards));
  }
  if (identity.total_shards < 1 || identity.shard_id >= identity.total_shards) {
    return std::unexpected(std::format(
        "invalid shard identity: id={}, total={}; require total >= 1 "
        "and 0 <= id < total",
        identity.shard_id, identity.total_shards));
  }
  if (auto w = db.write_shard_identity(identity); !w) {
    return std::unexpected(
        std::format("failed to persist shard identity: {}", w.error()));
  }
  if (auto c = db.commit(); !c) {
    return std::unexpected(
        std::format("failed to commit shard identity: {}", c.error()));
  }
  return identity;
}

} // namespace hogd
