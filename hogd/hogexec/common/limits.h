// limits.h - hogexec-enforced bounds on contract-controlled inputs.

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

// Bound on the serialized Contract row hogdb accepts in
// InvokeTX::request_new_contract(): blob bytes plus the XDR envelope (union
// tag 4 + opaque length 4 + padding <= 3), with slack. Exceeding it is
// executor misbehavior (kInvalidExecutorBehavior), not a storage limit --
// hogdb chunks any row up to this bound transparently.
inline constexpr size_t kMaxSerializedContractBytes =
    kMaxContractSizeBytes + 16;

} // namespace hogd
