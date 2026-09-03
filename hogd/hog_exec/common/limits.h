// limits.h - hog_exec-enforced bounds on contract-controlled inputs.

#pragma once

#include <cstddef>

#include "hogd/base/types.h"

namespace hogd {

// Maximum size of a deployed contract's code blob (ContractBlob.data for
// kRv32) -- the same quantity contract_deployment_cost_per_byte charges
// for. Enforced deterministically in
// SystemCallHandler::perform_initial_validation() before any attached
// contract is verified or executed (#2569).
//
// TODO(#2569): migrate into a protocol-defined config-bounds struct plumbed
// through genesis (like StorageCostModelXdr) once that exists.
inline constexpr size_t kMaxContractSizeBytes = 256_KiB;

// Maximum number of distinct contracts one transaction may attach in
// contracts_to_deploy. Enforced deterministically in
// SystemCallHandler::perform_initial_validation() before any attachment is
// verified (#3815).
//
// perform_initial_validation() verifies every distinct attachment outside
// transaction gas, and RV32 verification zero-fills the artifact's full
// declared guest-memory image (up to rv32::kMaxMemoryBytes). Without a count
// bound a size-compliant transaction can attach thousands of tiny artifacts
// that each declare the maximum image and force cumulative uncharged zero-fill
// far larger than the transaction itself. kMaxContractSizeBytes bounds each
// attachment; this bounds how many there are, so the worst-case uncharged
// verification work stays on the order of one maximum-size transaction rather
// than growing with the attachment count.
//
// TODO(#3815): migrate into the protocol network config
// (kConfigParamMaxNewContractsPerTx) once the submodule is bumped to a
// protocol that defines it, so a chain can state its own bound.
inline constexpr size_t kMaxNewContractsPerTx = 64;

// Bound on the serialized Contract row hog_db accepts in
// InvokeTX::request_new_contract(): blob bytes plus the XDR envelope (union
// tag 4 + opaque length 4 + padding <= 3), with slack. Exceeding it is
// executor misbehavior (kInvalidExecutorBehavior), not a storage limit --
// hog_db chunks any row up to this bound transparently.
inline constexpr size_t kMaxSerializedContractBytes =
    kMaxContractSizeBytes + 16;

} // namespace hogd
