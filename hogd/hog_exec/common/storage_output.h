// storage_output.h - the storage output a subtransaction carries, and the
// ceiling on max_subtransaction_output_bytes that keeps every subtransaction
// push within one executor->HogDb message.

#pragma once

#include <cstddef>
#include <cstdint>
#include <utility>

#include "address_auth.hh"
#include "hogd/base/log.h"
#include "hogd/hog_exec/common/limits.h"
#include "retrocausal_queue.hh"
#include "rpc_types.hh"
#include "storage_item.hh"
#include "types.hh"
#include "x23/rpc_msg.hh"
#include "x23/serialize.h"

namespace hogd {

// The storage output `delta` carries against max_subtransaction_output_bytes,
// as the protocol defines it beside kStorageOutputBytesPerDelta. Counts the
// values the delta carries, never their encoding, so every node computes the
// same figure however its link frames a batch. The flags and storage key are
// covered by the per-delta charge, so only the delta itself is read.
//
// Observed entries are not counted, and a delta staged only to carry them is
// not counted at all (PerTransactionDatabaseProxy::add_observation_delta):
// validators rebuild both from the proposal, and a proposer's batch has
// neither.
inline uint64_t
storage_output_bytes(StorageDelta const &delta)
{
  constexpr uint64_t kPerDelta = kStorageOutputBytesPerDelta;
  switch (delta.type()) {
  case StorageDeltaType::kDeltaTypeBytestringSet:
    return kPerDelta + delta.bytestring().size();
  case StorageDeltaType::kDeltaTypeRetrocausalQueue:
  {
    auto const &rq = delta.retrocausal_queue();
    uint64_t out = kPerDelta;
    auto charge = [&out](auto const &entries) {
      for (QueueEntry const &entry : entries) {
        out += uint64_t{kStorageOutputBytesPerQueueEntry} + entry.size();
      }
    };
    charge(rq.insert.added_entries);
    if (rq.consume) {
      charge(rq.consume->current_block_consumed);
    }
    return out;
  }
  case StorageDeltaType::kDeltaTypeAuthDataChange:
    return kPerDelta +
           (uint64_t{kStorageOutputBytesPerAuthSigner} *
            delta.auth_data_change().auth_data_change_signers.size());
  case StorageDeltaType::kDeltaTypeNonnegativeIntSetAdd:
  case StorageDeltaType::kDeltaTypeNonnegativeIntAdd:
  case StorageDeltaType::kDeltaTypeDelete:
  case StorageDeltaType::kDeltaTypeDeployContract:
  case StorageDeltaType::kDeltaTypeConsumeUniquifier:
    return kPerDelta;
  }
  panic("Unrecoverable; unknown StorageDeltaType: {}",
        std::to_underlying(delta.type()));
}

// Why max_subtransaction_output_bytes can bound what hogd sends HogDb. The
// protocol defines storage output apart from any encoding; hogd's XDR encoding
// must then never be larger, or a subtransaction the configuration admits
// could still overflow the executor->HogDb budget. The static_asserts below
// prove, arm by arm and for every value, that a delta encodes to at most its
// storage output plus its observed entries, whose count the protocol bounds
// separately (kMaxConcurrentRcqReadsPerTx). A layout change that breaks the
// proof fails to compile here rather than overflowing at run time.
namespace storage_output_detail {

template<typename T>
constexpr size_t kXdrMax = x23::detail::xdr_size_bounds<T>().max_;

// One RFC 4506 word: a length prefix, union discriminant or optional flag.
constexpr size_t kXdrWord = 4;

// The most XDR padding one opaque adds after its content.
constexpr size_t kXdrMaxPad = 3;

// Every delta encodes its key, flags and arm discriminant.
constexpr size_t kPerDeltaFraming = kXdrMax<AddressAndKey> +
                                    kXdrMax<StorageDeltaFlagSet> +
                                    kXdrMax<StorageDeltaType>;

constexpr size_t kPerDelta = kStorageOutputBytesPerDelta;

// Fixed-size arms fit the per-delta charge outright.
static_assert(kPerDeltaFraming + kXdrMax<NonnegativeInt128SetAddArgs> <=
              kPerDelta);
static_assert(kPerDeltaFraming + kXdrMax<int128_xdr> <= kPerDelta);
static_assert(kPerDeltaFraming + kXdrMax<DeployedContractHash> <= kPerDelta);
static_assert(kPerDeltaFraming + kXdrMax<ConsumeUniquifierArgs> <= kPerDelta);

// A bytestring adds its length prefix and padding to its content.
static_assert(kPerDeltaFraming + kXdrWord + kXdrMaxPad <= kPerDelta);

// An RCQ delta's fixed part: the past-block consume, the optional flag on the
// consume, and the length prefixes of its three entry arrays. Each entry adds
// its own prefix and padding.
constexpr size_t kRcqDeltaFixed =
    kXdrMax<RetrocausalQueuePastBlockConsumeDelta> + (4 * kXdrWord);
static_assert(kPerDeltaFraming + kRcqDeltaFixed <= kPerDelta);
static_assert(kXdrWord + kXdrMaxPad <= kStorageOutputBytesPerQueueEntry);

// An auth change's fixed part is everything but its signers.
constexpr size_t kAuthChangeFixed =
    kXdrMax<AuthDataChangeArgs> -
    (size_t{kMaxSigners} * kXdrMax<AuthDataChangeSigner>);
static_assert(kPerDeltaFraming + kAuthChangeFixed <= kPerDelta);
static_assert(kXdrMax<AuthDataChangeSigner> <=
              kStorageOutputBytesPerAuthSigner);

// Observed entries carry no storage output, so they are bounded by count:
// charged as if each sat alone in an uncounted observation-only delta.
constexpr size_t kObservationBytesBound =
    size_t{kMaxConcurrentRcqReadsPerTx} *
    (kPerDeltaFraming + kRcqDeltaFixed + kXdrMax<QueueEntry>);

// Bytes of a try_push_new_subtransaction request outside its deltas: the
// rpc_msg header, the BlockID and TransactionID arguments, and the batch's
// length prefix.
constexpr size_t kPushMessageOverhead = kXdrMax<x23::rpc_msg> +
                                        kXdrMax<BlockID> +
                                        kXdrMax<TransactionID> + kXdrWord;

// The XDR bytes of the observed entries `delta` carries, which storage output
// leaves out; zero for any arm but RCQ.
inline size_t
observed_entries_xdr_size(StorageDelta const &delta)
{
  if (delta.type() != StorageDeltaType::kDeltaTypeRetrocausalQueue) {
    return 0;
  }
  size_t out = 0;
  for (QueueEntry const &entry :
       delta.retrocausal_queue().observed_concurrent_entries) {
    out += x23::xdr_size(entry);
  }
  return out;
}

} // namespace storage_output_detail

// The largest max_subtransaction_output_bytes this build can honor: a
// subtransaction at the limit, plus the most observations a validator can
// rebuild into it, still fits one executor->HogDb message. Genesis load
// rejects a configuration above it.
inline constexpr size_t kSubtransactionOutputCeilingBytes =
    kExecToDbMessageCap - storage_output_detail::kPushMessageOverhead -
    storage_output_detail::kObservationBytesBound;

static_assert(storage_output_detail::kPushMessageOverhead +
                  storage_output_detail::kObservationBytesBound <
              kExecToDbMessageCap);
static_assert(kDefaultMaxSubtransactionOutputBytes <=
                  kSubtransactionOutputCeilingBytes,
              "the protocol default must fit this build's HogDb link");

} // namespace hogd
