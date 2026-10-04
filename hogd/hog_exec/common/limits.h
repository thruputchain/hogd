// limits.h - hog_exec-enforced bounds on contract-controlled inputs.

#pragma once

#include <cstddef>

#include "auth_token.hh"
#include "hog_exec_constants.hh"
#include "hogd/base/types.h"
#include "integration.hh"
#include "x23/serialize.h"

namespace hogd {

// Cap on the Transaction body, what TransactionHash commits to (#3844).
inline constexpr size_t kMaxBytesTransaction = 1'900'000;
static_assert(kMaxBytesTransaction < kMaxBytesSignedTransaction);

// Ceiling on kConfigParamMaxSavepointDepth, checked at genesis load: each open
// savepoint is a nested VM on one executor thread's stack, and this is the
// depth those stacks are sized for.
inline constexpr uint32_t kSavepointDepthCeiling =
    static_cast<uint32_t>(kMaxSavepointDepth);

// Ceiling on kConfigParamMaxAuthTokenDepth, checked at genesis load. A view
// query's forwarded tokens are decoded by the RPC layer, under x23's default
// recursion limit (one level per AuthTokenNode), before the executor knows the
// chain's depth; a chain allowing deeper tokens would mint tokens no view query
// could carry. It also bounds how deep MintAuthToken's decode recurses.
inline constexpr uint32_t kAuthTokenDepthCeiling =
    static_cast<uint32_t>(x23::kDefaultDeserializationRecursionLimit);
static_assert(static_cast<uint32_t>(kAuthTokenMaxDepth) <=
              kAuthTokenDepthCeiling);

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

} // namespace hogd
