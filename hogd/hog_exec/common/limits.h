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

// Bound on the serialized Contract row hog_db accepts in
// InvokeTX::request_new_contract(): blob bytes plus the XDR envelope (union
// tag 4 + opaque length 4 + padding <= 3), with slack. Exceeding it is
// executor misbehavior (kInvalidExecutorBehavior), not a storage limit --
// hog_db chunks any row up to this bound transparently.
inline constexpr size_t kMaxSerializedContractBytes =
    kMaxContractSizeBytes + 16;

// Maximum number of simultaneously-live invocation frames (i.e. the ordinary +
// TryInvoke call-nesting depth). Enforced deterministically in
// SystemCallHandler::validate_new_invocation() before a child frame is created.
//
// The exact-address reentrance guard only blocks reusing an address already on
// the stack, and the TryInvoke savepoint cap (kMaxSavepointDepth) bounds
// sequential TryInvoke breadth rather than nesting depth, so ordinary
// invocation nesting was previously unbounded. That let a contract build a deep
// address-distinct call chain whose per-frame first-use item lookups scan every
// ancestor frame and whose successful unwinds copy each child's write set into
// its parent -- host work that grows with depth times modified-item count but
// is not gas-metered (hogd issue #3590). Capping the depth makes that per-write
// factor a constant, so the total host work stays linear in the metered work,
// and also bounds the native call-stack recursion each nested invoke consumes.
//
// TODO(#2569): migrate into a protocol-defined config-bounds struct plumbed
// through genesis (like StorageCostModelXdr) once that exists.
inline constexpr size_t kMaxInvocationDepth = 128;

} // namespace hogd
