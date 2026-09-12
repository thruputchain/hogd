// limits.h - hog_exec-enforced bounds on contract-controlled inputs.

#pragma once

#include <cstddef>

#include "hog_exec_constants.hh"
#include "hogd/base/types.h"
#include "integration.hh"

namespace hogd {

// Cap on the Transaction body, what TransactionHash commits to (#3844).
inline constexpr size_t kMaxBytesTransaction = 1'900'000;
static_assert(kMaxBytesTransaction < kMaxBytesSignedTransaction);

// The most storage savepoints a chain may configure this build to hold
// (kConfigParamMaxSavepointDepth), checked when a genesis is loaded. Every
// open savepoint is a TryInvoke frame still executing, and each of those is a
// nested VM plus its coroutine frames on one executor thread's stack, so the
// ceiling is what those stacks are known to sustain: the depth every chain ran
// under before it was configurable. Raising it is a stack-sizing decision for
// the executor, not a configuration one.
inline constexpr uint32_t kSavepointDepthCeiling =
    static_cast<uint32_t>(kMaxSavepointDepth);

// The budget the executor holds itself to on the HogDb link (pinned to
// kBulkRpcMaxMessageSize). Raising it changes which transactions abort.
inline constexpr size_t kExecToDbMessageCap = 0x100'0000;

// The largest contract code blob (ContractBlob.data for kRv32) a chain may
// configure this build to admit: the ceiling on
// kConfigParamMaxContractSizeBytes, checked when a genesis is loaded. The
// limit in force is the configured one, enforced deterministically in
// SystemCallHandler::perform_initial_validation() before any attached
// contract is verified or executed (#2569); this bound only says how far a
// chain may raise it.
//
// A transaction body carries its attachments, so a blob above
// kMaxBytesTransaction could never reach the executor and nothing is lost by
// refusing to configure one. Everything below it must then hold up: hog_db
// must store a row of this size (the chunk-count static_assert in
// InvokeTX::request_new_contract), and the commit batch
// max_new_contracts_per_tx bounds must fit one executor->HogDb message
// (checked against the pair at genesis load, and for one contract below).
// Raising kMaxBytesTransaction past what either can carry fails to compile
// rather than admitting a contract no node could commit.
inline constexpr size_t kContractSizeCeilingBytes = kMaxBytesTransaction;

// Bytes the XDR envelope adds to a blob in a serialized Contract row: union
// tag 4 + opaque length 4 + padding <= 3, with slack.
inline constexpr size_t kSerializedContractEnvelopeBytes = 16;

// Bound on the serialized Contract row holding a blob of `blob_bytes`.
constexpr size_t
serialized_contract_bound(size_t blob_bytes)
{
  return blob_bytes + kSerializedContractEnvelopeBytes;
}

// Bound on the serialized Contract row hog_db accepts in
// InvokeTX::request_new_contract(): the row for a blob at the ceiling.
// Exceeding it is executor misbehavior (kInvalidExecutorBehavior), not a
// storage limit -- hog_db chunks any row up to this bound transparently.
inline constexpr size_t kMaxSerializedContractBytes =
    serialized_contract_bound(kContractSizeCeilingBytes);

// Even a chain admitting one contract per transaction needs that contract's
// commit to fit one message.
static_assert(kMaxSerializedContractBytes + 1_KiB <= kExecToDbMessageCap,
              "a contract at the ceiling must fit one executor->HogDb message");

} // namespace hogd
