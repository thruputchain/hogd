#pragma once

#include <memory>

#include "hogd/base/xdr_util.h"
#include "hogd/hogexec/runtime/wasmi_runtime.h"

namespace hogd {

// Maintain state of one actively-running deployment.
struct RuntimeCacheEntry {
  WasmiRuntime runtime_;
  // is this entry created by a deployment that is new in the current
  // transaction
  const bool local_deployment_;

  // When a call frame reverts, it reverts all of the state changes
  // made in that frame (and all child frames).
  // We do not revert the internal details of individual WasmiRuntimes
  // (we could, in theory; but this is expensive and would require
  // very careful management of Wasmi's internal data structures).
  // Instead, we mark any contract that was reverted as `revert_poisoned`,
  // and prevent any further invocations on that runtime.
  // Future PRs will allow metadata to allow a contract to mark itself
  // as `revert_safe`.
  bool revert_poisoned_ = false;

  RuntimeCacheEntry(WasmiRuntime &&runtime, bool local_deployment)
    : runtime_(std::move(runtime)), local_deployment_(local_deployment)
  {}

  // Disable move and copy.
  RuntimeCacheEntry(RuntimeCacheEntry &&ll) = delete;
  RuntimeCacheEntry &operator=(RuntimeCacheEntry &&ll) = delete;
  RuntimeCacheEntry(const RuntimeCacheEntry &ll) = delete;
  RuntimeCacheEntry &operator=(const RuntimeCacheEntry &ll) = delete;
};

// RuntimeCacheEntry cannot be moved while it is actively invoked.  unique_ptr
// thus required here.
using RuntimeCache =
    std::unordered_map<ContractAddress, std::unique_ptr<RuntimeCacheEntry>>;

} // namespace hogd
