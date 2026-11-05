// limits.h - hog_exec-enforced bounds on contract-controlled inputs.

#pragma once

#include <cstddef>

#include "event.hh"
#include "hog_exec_constants.hh"
#include "hogd/base/types.h"
#include "integration.hh"

namespace hogd {

// Cap on the Transaction body, what TransactionHash commits to (#3844).
inline constexpr size_t kMaxBytesTransaction = 1'900'000;
static_assert(kMaxBytesTransaction < kMaxBytesSignedTransaction);

// Ceiling on kConfigParamMaxSavepointDepth, checked at genesis load: each open
// savepoint is a nested VM on one executor thread's stack, and this is the
// depth those stacks are sized for.
inline constexpr uint32_t kSavepointDepthCeiling =
    static_cast<uint32_t>(kMaxSavepointDepth);

// The budget the executor holds itself to on the HogDb link (pinned to
// kBulkRpcMaxMessageSize). Raising it changes which transactions abort.
inline constexpr size_t kExecToDbMessageCap = 0x100'0000;

// Ceiling on kConfigParamMaxContractSizeBytes, checked at genesis load. A
// transaction body carries its attachments, so nothing above
// kMaxBytesTransaction could reach the executor; hog_db stores rows up to it,
// and the commit-batch check below keeps one such contract in one message.
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
// InvokeTX::request_new_contract(); exceeding it is executor misbehavior.
inline constexpr size_t kMaxSerializedContractBytes =
    serialized_contract_bound(kContractSizeCeilingBytes);

// One contract at the ceiling must fit one executor->HogDb message.
static_assert(kMaxSerializedContractBytes + 1_KiB <= kExecToDbMessageCap,
              "a contract at the ceiling must fit one executor->HogDb message");

// The budget the executor holds itself to on the event link (EventServerProxy's
// default). Raising it requires raising the event server's cap too.
inline constexpr size_t kExecToEventMessageCap = 0x100'0000;

// Ceiling on kConfigParamMaxReturnBufferLen, checked at genesis load. A root
// frame's buffer rides in TxEventLog::root_output on the event link, next to
// the signed transaction and the transaction's events, so all three must fit
// one message (#4744).
inline constexpr uint32_t kReturnBufferCeilingLen = 8 * 1024 * 1024;

// Each event adds its address, topic count, and length prefix to its data.
static_assert(kReturnBufferCeilingLen + kMaxBytesSignedTransaction +
                      (kMaxEventsPerTransaction *
                       (sizeof(ContractAddress) + 8 + kMaxEventLength)) +
                      1_KiB <=
                  kExecToEventMessageCap,
              "a maximal root return, signed transaction and event list must "
              "fit one executor->event message");

} // namespace hogd
