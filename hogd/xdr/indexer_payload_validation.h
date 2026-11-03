#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>

#include "event.hh"
#include "indexer.hh"
#include "types.hh"
#include "x23/async.h"
#include "x23/vec.h"

namespace hogd {

inline constexpr uint32_t kIndexerOutboxFormatVersion = 3;
// x23's unchunked record marker has 31 payload-length bits. Keep this limit
// local to the indexer replay path instead of narrowing unrelated bulk RPCs.
inline constexpr size_t kIndexerRpcMaxMessageSize = (size_t{1} << 31) - 1;
template<typename T> using IndexerPayloadStatus = std::expected<T, std::string>;

// Canonical membership is bounded and strictly increasing by raw address bytes.
bool
valid_invoked_addresses(std::span<const ContractAddress> addresses) noexcept;

struct IndexerPayloadRoots {
  EventsHash events_hash;
  TxsHash txs_hash;
};

// Serializes the canonical event-trie leaf preimage into buf and returns the
// byte length of the TxEventLog prefix. The leaf key is the first 32 bytes; its
// value is the remainder of the event followed by nondeterministic data.
size_t serialize_event_commitment_leaf(
    x23::Buf &buf, const TxEventLog &event,
    const NondeterministicProposalData &nondeterministic_data);

IndexerPayloadStatus<IndexerPayloadRoots>
compute_indexer_payload_roots(const TxEventLogBatch &event_logs,
                              const ProposedTransactionBatch &tx_bodies);
EventsHash
compute_indexer_outbox_event_root(const TxEventLogBatch &event_logs,
                                  const ProposedTransactionBatch &tx_bodies);
// The txset root [ProposalHeader::txs_hash] commits to: a Merkle trie over
// the [TransactionHash] of each of [tx_bodies]. [ProposalTxs] names the same
// txs by [SignedTransactionHash], so this root is not recomputable from it.
IndexerPayloadStatus<TxsHash>
compute_indexer_txs_root(const ProposedTransactionBatch &tx_bodies);
x23::Async<IndexerPayloadStatus<void>>
validate_indexer_committed_payload(const IndexerCommittedBlockPayload &payload);
x23::Async<IndexerPayloadStatus<IndexerCommittedBlock>>
make_indexer_committed_block(const IndexerCommittedBlockPayload &payload);
x23::Async<IndexerPayloadStatus<void>>
validate_indexer_committed_block(const IndexerCommittedBlock &block);

} // namespace hogd
