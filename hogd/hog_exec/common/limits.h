// limits.h - hog_exec-enforced bounds on contract-controlled inputs.

#pragma once

#include <cstddef>

#include "hogd/base/rpc_message_size.h"
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

// What the executor->HogDb link will carry in one message. x23 bounds
// (record.size() - 4) -- the rpc_msg header plus the call args -- against
// max_message_size (see send_call in x23/rpc.h).
//
// This link carries a transaction's whole final commit batch in one message,
// so it is a bulk path and takes the same bound as the others (mempool, auth,
// coordinator, indexer). Both ends bind here: the client in database_proxy.cc
// and the servers in rpc_from_exec.h and db_executor_server.h. It previously
// mirrored x23's default_max_message_size, a runtime-mutable extern that no
// production code set, leaving this the one bulk hop still at 16 MiB while
// admission ran three orders of magnitude higher.
inline constexpr size_t kExecToDbTransportCap = kBulkRpcMaxMessageSize;

// What the executor holds itself to on that link, which is well below what the
// link would carry.
//
// Kept at the value the transport used to impose rather than raised with it.
// The subtransaction-push gate turns an over-budget batch into a conflict, so
// this decides which transactions abort -- raising it would make nodes accept
// batches their peers on an older build reject, which is a consensus-visible
// change and belongs in its own change with its own argument. Widening the
// transport, by contrast, only stops a message the executor already meant to
// send from failing at the wire.
inline constexpr size_t kExecToDbMessageCap = 0x100'0000;
static_assert(kExecToDbMessageCap <= kExecToDbTransportCap,
              "the executor's own budget must fit what the link will carry");

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
