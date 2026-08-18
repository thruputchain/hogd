#pragma once

#include <cstdint>
#include <expected>
#include <optional>
#include <string>

#include "sched_db.hh" // ShardIdentity

namespace hogd {

class HogDBDAO;

// Resolve which shard a ramdb directory is, persisting the answer on a db that
// does not carry one yet. A persisted record is authoritative and the flags
// only confirm it, so relabelling a populated db is fail-stop instead of
// silently re-routing its keys. Shared by hogdb and hog-devnode so both agree
// on what a given directory holds.
//
// Returns the resolved identity, or a message the caller fail-stops on.
std::expected<ShardIdentity, std::string>
establish_shard_identity(HogDBDAO &db,
                         std::optional<std::uint32_t> flag_shard_id,
                         std::optional<std::uint32_t> flag_total_shards);

} // namespace hogd
