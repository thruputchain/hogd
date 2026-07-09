#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>

#include "types.hh" // AddressAndKey, ContractAddress

namespace hogd {

// The primary shard. Global/keyless requests (db tip, viewing snapshots) and
// content-addressed lookups (contracts) route here for now.
inline constexpr size_t kPrimaryShard = 0;

// Canonical key->shard routing for a database sharded into `shard_count`
// shards. This is the single source of truth for the mapping; both the
// executor and (eventually) any other sharding-aware component should route
// through here rather than reimplementing it.
//
// Routes by account address so that all of an address's storage items
// co-locate on a single shard. Deterministic within a process.
//
// NOTE: The hash is a placeholder (FNV-1a over the 32 address bytes). A
// network-agreed function will replace it when sharding becomes externally
// visible; keeping this the only definition makes that swap a single edit.
//
// Precondition: `shard_count >= 1`. Returns a value in [0, shard_count).
inline size_t shard_of_address(ContractAddress const &address,
                               size_t shard_count)
{
  assert(shard_count >= 1);
  uint64_t h = 1469598103934665603ULL; // FNV-1a offset basis
  for (uint8_t byte : address) {
    h ^= byte;
    h *= 1099511628211ULL; // FNV-1a prime
  }
  return static_cast<size_t>(h % shard_count);
}

// Convenience overload: routes an AddressAndKey by its account address.
inline size_t shard_of_key(AddressAndKey const &key, size_t shard_count)
{
  return shard_of_address(key.address, shard_count);
}

} // namespace hogd
