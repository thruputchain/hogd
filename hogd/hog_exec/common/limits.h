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

// Maximum number of DISTINCT contract entries a single transaction may cause
// the block-scoped StorageSnapshotCache to warm. Warming a distinct contract
// fetches, deserializes, and retains a full ContractCacheEntry (code up to
// kMaxContractSizeBytes) until the block's snapshot cache is extracted. Two
// contract-controlled paths warm entries before the transaction pays for the
// work: invoke() callee resolution (SystemCallHandler::resolve_runtime), which
// runs before the child frame spends gas -- even for gas_limit=0 / TryInvoke,
// which faults the child without charging child gas -- and DeployContract
// (SystemCallHandler::deploy_contract), which loads an existing contract by
// hash before the deploy commits and even if the transaction reverts. Without
// a bound, either lets a contract pin retained code bytes out of proportion to
// the gas it spends (#3375).
//
// Both paths draw on this single per-transaction budget (see
// SystemCallHandler::distinct_contract_loads()) and reject a new distinct load
// before the cold fetch/translate allocates, so a transaction's retained code
// contribution is bounded at kMaxDistinctContractResolutionsPerTx *
// kMaxContractSizeBytes (64 MiB here). Enforcement is deterministic per
// transaction, so the bound holds identically in proposal and validation, and
// the limit is generous relative to any legitimate transaction, which touches
// few distinct contracts.
inline constexpr size_t kMaxDistinctContractResolutionsPerTx = 256;

// Bound on the serialized Contract row hog_db accepts in
// InvokeTX::request_new_contract(): blob bytes plus the XDR envelope (union
// tag 4 + opaque length 4 + padding <= 3), with slack. Exceeding it is
// executor misbehavior (kInvalidExecutorBehavior), not a storage limit --
// hog_db chunks any row up to this bound transparently.
inline constexpr size_t kMaxSerializedContractBytes =
    kMaxContractSizeBytes + 16;

} // namespace hogd
