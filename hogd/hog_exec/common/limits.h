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

// The executor->HogDb RPC message cap. x23 bounds (record.size() - 4) -- the
// rpc_msg header plus the call args -- against max_message_size (see send_call
// in x23/rpc.h). exec_server runs at x23's default_max_message_size, which is a
// runtime-mutable extern (tests lower it) with no constexpr to bind to; this
// mirrors its compile-time default.
// TODO(rstutsman): bind to an x23 constexpr default once x23 exposes one.
inline constexpr size_t kExecToDbMessageCap = 0x100'0000;

// Bytes of the commit_transaction request that share the message with the
// contract batch but are not part of it: the rpc_msg header, the BlockID and
// TransactionID args, and the batch's Vec length prefix. The real total is a
// few dozen bytes; rounding up to a KiB costs a chain nothing against a cap
// measured in MiB and keeps the bound honest if the request gains an argument.
inline constexpr size_t kCommitBatchMessageOverhead = 1_KiB;

/// The most contracts a transaction may attach and still be certain to commit
/// under `message_cap`.
///
/// A transaction's deployed contract bodies are sent to storage as one
/// message, and each is bounded by kMaxSerializedContractBytes, so this is the
/// count at which even an all-max-size batch fits. A chain's
/// NetworkConfig::max_new_contracts_per_tx must not exceed it -- otherwise the
/// chain admits transactions whose commit its own storage link cannot carry,
/// which surfaces as an unrecoverable execution result rather than a
/// transaction failure.
constexpr size_t
max_new_contracts_for_message_cap(size_t message_cap)
{
  if (message_cap <= kCommitBatchMessageOverhead) {
    return 0;
  }
  return (message_cap - kCommitBatchMessageOverhead) /
         kMaxSerializedContractBytes;
}

} // namespace hogd
