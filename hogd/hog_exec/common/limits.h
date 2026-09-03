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

// Bound on the serialized Contract row hog_db accepts in
// InvokeTX::request_new_contract(): blob bytes plus the XDR envelope (union
// tag 4 + opaque length 4 + padding <= 3), with slack. Exceeding it is
// executor misbehavior (kInvalidExecutorBehavior), not a storage limit --
// hog_db chunks any row up to this bound transparently.
inline constexpr size_t kMaxSerializedContractBytes =
    kMaxContractSizeBytes + 16;

// The executor->HogDb RPC message cap: x23 bounds (record.size() - 4) against
// max_message_size (see send_call in x23/rpc.h). The link itself is pinned to
// kBulkRpcMaxMessageSize at both ends; this is the smaller budget the executor
// holds itself to, kept at the value the transport used to impose because the
// subtransaction-push gate turns an over-budget batch into a conflict, so
// raising it would change which transactions abort.
inline constexpr size_t kExecToDbMessageCap = 0x100'0000;

} // namespace hogd
