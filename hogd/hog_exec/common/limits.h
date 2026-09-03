// limits.h - hog_exec-enforced bounds on contract-controlled inputs.

#pragma once

#include <cstddef>
#include <cstdint>

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

// Bound on the serialized Contract row hog_db accepts in
// InvokeTX::request_new_contract(): blob bytes plus the XDR envelope (union
// tag 4 + opaque length 4 + padding <= 3), with slack. Exceeding it is
// executor misbehavior (kInvalidExecutorBehavior), not a storage limit --
// hog_db chunks any row up to this bound transparently.
inline constexpr size_t kMaxSerializedContractBytes =
    kMaxContractSizeBytes + 16;

// Maximum number of MintAuthToken attempts a single transaction may make. Each
// attempt deserializes a contract-controlled tree and, on success, retains the
// decoded token in the per-transaction auth-token registry -- all for the flat
// syscall cost -- so without a bound a contract could force unbounded retained
// state and repeated decode/traversal work (including with trees that fail the
// depth check or deserialize only after substantial parsing). The registry's
// own ceiling is UINT32_MAX ids -- far too large to be a practical cap -- so
// this pins a finite, generous per-transaction limit. It is counted per
// attempt before the parse (AuthTokenRegistry::try_begin_mint_attempt),
// checked in the MintAuthToken syscall body; unwrapping an existing token into
// its children is not a mint and does not count against it.
inline constexpr uint32_t kMaxAuthTokensPerTransaction = 1024;

} // namespace hogd
