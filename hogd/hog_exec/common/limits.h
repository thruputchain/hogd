// limits.h - hog_exec-enforced bounds on contract-controlled inputs.

#pragma once

#include <cstddef>

#include "hogd/base/types.h"

namespace hogd {

// Maximum size of a deployed contract's code blob (ContractBlob.data for
// kWasmi / kRv32) -- the same quantity contract_deployment_cost_per_byte
// charges for. Enforced deterministically in
// SystemCallHandler::perform_initial_validation() before any attached
// contract is verified or executed (#2569).
//
// TODO(#2569): migrate into a protocol-defined config-bounds struct plumbed
// through genesis (like StorageCostModelXdr) once that exists.
inline constexpr size_t kMaxContractSizeBytes = 256_KiB;

// Maximum number of DISTINCT deployed contracts a single transaction may
// resolve. The first resolution of a distinct callee warms a full
// ContractCacheEntry (code up to kMaxContractSizeBytes) into the block-scoped
// StorageSnapshotCache, where it is retained until the block's snapshot cache
// is extracted. Callee resolution happens before the child frame runs or
// spends gas, so a contract can invoke many distinct already-deployed callees
// -- even with gas_limit=0 and TryInvoke, which faults the child without
// charging child gas -- and pin retained code bytes out of proportion to the
// gas it actually spends (#3375). Bounding the count of distinct resolutions
// per transaction bounds a single transaction's retained code contribution at
// kMaxDistinctContractResolutionsPerTx * kMaxContractSizeBytes (64 MiB here).
//
// Enforced deterministically per transaction in
// SystemCallHandler::resolve_runtime(), before the cold fetch/translate
// allocates, so the bound holds identically in proposal and validation. The
// limit is generous relative to any legitimate transaction, which resolves few
// distinct callees.
inline constexpr size_t kMaxDistinctContractResolutionsPerTx = 256;

// Bound on the serialized Contract row hog_db accepts in
// InvokeTX::request_new_contract(): blob bytes plus the XDR envelope (union
// tag 4 + opaque length 4 + padding <= 3), with slack. Exceeding it is
// executor misbehavior (kInvalidExecutorBehavior), not a storage limit --
// hog_db chunks any row up to this bound transparently.
inline constexpr size_t kMaxSerializedContractBytes =
    kMaxContractSizeBytes + 16;

} // namespace hogd
