#pragma once

#include "contract.hh" // DeployedContractHash

namespace hogd {

// Defined in storage_snapshot_cache.h. DeployedContract only holds a pointer
// to it, so a forward declaration suffices here and keeps this header free of
// any rv32 dependency (avoids a runtime.h <-> storage_snapshot_cache.h cycle).
struct ContractCacheEntry;

// A deployed contract: its on-ledger hash metadata plus a borrowed pointer to
// the cached contract entry (code + translation). `entry` is null for
// transaction-local deployments, which are not in the snapshot cache.
struct DeployedContract {
  DeployedContractHash hash;
  const ContractCacheEntry *entry = nullptr;

  // Implicit: lets the many make_new_runtime(DeployedContractHash, ...) call
  // sites and the tx-local fallback construct a DeployedContract with no entry.
  DeployedContract(DeployedContractHash hash) : hash(hash) {}
  DeployedContract(DeployedContractHash hash, const ContractCacheEntry *entry)
    : hash(hash), entry(entry)
  {}
};

} // namespace hogd
