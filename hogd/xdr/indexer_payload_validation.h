#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>

#include "indexer.hh"
#include "proposal.hh"
#include "types.hh"
#include "x23/async.h"

namespace hogd {

inline constexpr uint32_t kIndexerOutboxFormatVersion = 1;
// x23's unchunked record marker has 31 payload-length bits. Keep this limit
// local to the indexer replay path instead of narrowing unrelated bulk RPCs.
inline constexpr size_t kIndexerRpcMaxMessageSize = (size_t{1} << 31) - 1;
template<typename T> using IndexerPayloadStatus = std::expected<T, std::string>;

SHA256Hash compute_indexer_outbox_event_root(const TxEventLogBatch &event_logs);
x23::Async<IndexerPayloadStatus<TxsHash>>
compute_indexer_proposal_txs_root(const ProposalTxs &proposal_txs);
x23::Async<IndexerPayloadStatus<void>>
validate_indexer_committed_payload(const IndexerCommittedBlockPayload &payload);
x23::Async<IndexerPayloadStatus<IndexerCommittedBlock>>
make_indexer_committed_block(const IndexerCommittedBlockPayload &payload);
x23::Async<IndexerPayloadStatus<void>>
validate_indexer_committed_block(const IndexerCommittedBlock &block);

} // namespace hogd
