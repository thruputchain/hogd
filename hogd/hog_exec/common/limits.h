// limits.h - hog_exec-enforced bounds on contract-controlled inputs.

#pragma once

#include <cstddef>

#include "auth_claim.hh"
#include "hogd/base/types.h"
#include "integration.hh"
#include "transaction.hh"
#include "x23/serialize.h"

namespace hogd {

// Every signature a SignedTransaction can carry once auth has admitted it.
// verify_auth accepts at most kMaxSigners signatures (strictly increasing key
// ids drawn from a kMaxSigners-bit bitmap, or exactly one for a pubkey claim),
// and a DecoratedSignature has a fixed encoded size, so this is the XDR length
// prefix plus kMaxSigners encoded signatures.
static_assert(x23::detail::has_fixed_size<DecoratedSignature>());
inline constexpr size_t kMaxBytesSignatureVector =
    4 + (kMaxSigners * x23::detail::xdr_size_bounds<DecoratedSignature>().max_);

// Size cap on a Transaction body -- the bytes TransactionHash commits to.
// Enforced deterministically by ExecutorContext in both proposal and
// validation, before the handler is built: an over-cap body is malformed and
// is dropped (ExecResultMalformedTransaction when proposing,
// ExecResultTxValidationFailure when validating), never included in a block.
//
// The cap is deliberately on the body and not on the SignedTransaction
// wrapper. The hash excludes the signatures, so the wrapper a node holds for a
// given hash is node-local state: two valid signature vectors over the same
// body can straddle any wrapper-level cap, and a proposer holding the smaller
// wrapper would nominate a hash that a validator holding the larger one
// rejects (#3844). Sizing the body cap so that a body at the cap plus the
// largest signature vector auth admits is exactly kMaxBytesSignedTransaction
// keeps the protocol's wrapper bound intact for every wrapper that can reach
// the executor, without the executor ever examining the wrapper.
inline constexpr size_t kMaxBytesTransaction =
    kMaxBytesSignedTransaction - kMaxBytesSignatureVector;

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

// The budget the executor holds itself to on the HogDb link (pinned to
// kBulkRpcMaxMessageSize). Raising it changes which transactions abort.
inline constexpr size_t kExecToDbMessageCap = 0x100'0000;

} // namespace hogd
