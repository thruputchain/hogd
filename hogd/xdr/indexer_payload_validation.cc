#include "hogd/xdr/indexer_payload_validation.h"

#include <cstddef>
#include <expected>
#include <format>
#include <span>

#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h"
#include "hogd/container/merkle/allocator.h"
#include "hogd/container/merkle/hex_array.h"
#include "hogd/container/merkle/merkle_trie.h"
#include "hogd/container/merkle/node.h"
#include "hogd/crypto/crypto.h"
#include "indexer.hh"
#include "proposal.hh"
#include "transaction.hh"
#include "types.hh"
#include "x23/async.h"
#include "x23/serialize.h"
#include "x23/vec.h"

namespace hogd {
namespace {

IndexerPayloadStatus<TxsHash>
validate_event_body_mapping_and_compute_root(
    const TxEventLogBatch &event_logs,
    const ProposedTransactionBatch &tx_bodies)
{
  if (event_logs.size() != tx_bodies.size()) {
    return std::unexpected("event/body batch cardinalities differ");
  }

  MerkleTriePrefix trie(/*allocator_type=*/Allocator<Node>::Type::kBump);
  for (size_t i = 0; i < tx_bodies.size(); ++i) {
    const TransactionHash body_hash = sha256(tx_bodies[i].signed_tx.tx);
    if (body_hash != event_logs[i].tx_hash) {
      return std::unexpected(std::format(
          "transaction body hash differs from event log at index {}", i));
    }
    if (!trie.insert(HexArray(body_hash), std::span<const std::byte>())) {
      return std::unexpected("event/body batch contains duplicate hashes");
    }
  }
  trie.generate_all_hashes(/*num_threads=*/1);
  return trie.get_root_hash();
}

} // namespace

SHA256Hash
compute_indexer_outbox_event_root(const TxEventLogBatch &event_logs)
{
  MerkleTriePrefix trie(/*allocator_type=*/Allocator<Node>::Type::kBump);
  for (const TxEventLog &event_log : event_logs) {
    x23::Buf serialized = x23::serialize(event_log);
    std::span<const std::byte> bytes = std::as_bytes(std::span(serialized));
    CHECK(trie.insert_or_update(bytes.first<32>(), bytes.subspan(32)));
  }
  trie.generate_all_hashes(/*num_threads=*/1);
  return trie.get_root_hash();
}

x23::Async<IndexerPayloadStatus<TxsHash>>
compute_indexer_proposal_txs_root(const ProposalTxs &proposal_txs)
{
  x23::XdrVec<TransactionHash> hashes;
  if (!(co_await parallel_batch_to_t(proposal_txs.hash, hashes))) {
    co_return std::unexpected("malformed canonical transaction hash batch");
  }
  MerkleTriePrefix trie(/*allocator_type=*/Allocator<Node>::Type::kBump);
  for (const TransactionHash &hash : hashes) {
    if (!trie.insert(HexArray(hash), std::span<const std::byte>())) {
      co_return std::unexpected(
          "canonical transaction hash batch has duplicates");
    }
  }
  trie.generate_all_hashes(/*num_threads=*/1);
  co_return trie.get_root_hash();
}

x23::Async<IndexerPayloadStatus<void>>
validate_indexer_committed_payload(const IndexerCommittedBlockPayload &payload)
{
  if (payload.format_version != kIndexerOutboxFormatVersion) {
    co_return std::unexpected(
        std::format("unknown committed outbox format version {}",
                    payload.format_version));
  }
  if (payload.consensus_value.proposal_header.block_height < 0) {
    co_return std::unexpected("committed outbox height is negative");
  }
  if (compute_indexer_outbox_event_root(payload.event_logs) !=
      payload.consensus_value.proposal_header.events_hash) {
    co_return std::unexpected("event root differs from payload identity");
  }
  // ProposalTxsHash intentionally preserves ParallelBatch segmentation, so it
  // cannot be reconstructed from tx_bodies without retaining the duplicate
  // ProposalTxs preimage. The finalized scheduler/executor path is trusted for
  // nondeterministic proposal data; the payload digest detects durable
  // corruption. We still independently bind every inner Transaction to the
  // consensus transaction-set root below; signatures share the trusted
  // internal-data boundary because TransactionHash excludes them.
  auto txs_root =
      validate_event_body_mapping_and_compute_root(payload.event_logs,
                                                   payload.tx_bodies);
  if (!txs_root) {
    co_return std::unexpected(txs_root.error());
  }
  if (*txs_root != payload.consensus_value.proposal_header.txs_hash) {
    co_return std::unexpected(
        "canonical transaction root differs from ProposalHeader");
  }
  co_return IndexerPayloadStatus<void>{};
}

x23::Async<IndexerPayloadStatus<IndexerCommittedBlock>>
make_indexer_committed_block(const IndexerCommittedBlockPayload &payload)
{
  if (auto valid = co_await validate_indexer_committed_payload(payload);
      !valid) {
    co_return std::unexpected(valid.error());
  }
  co_return IndexerCommittedBlock{
      .payload = payload,
      .identity =
          IndexerCommittedBlockIdentity{
              .consensus_hash = sha256(payload.consensus_value),
              .payload_digest = sha256(payload),
          },
  };
}

x23::Async<IndexerPayloadStatus<void>>
validate_indexer_committed_block(const IndexerCommittedBlock &block)
{
  if (auto valid = co_await validate_indexer_committed_payload(block.payload);
      !valid) {
    co_return valid;
  }
  if (block.identity.consensus_hash != sha256(block.payload.consensus_value) ||
      sha256(block.payload) != block.identity.payload_digest) {
    co_return std::unexpected(
        "committed block identity differs from canonical payload");
  }
  co_return IndexerPayloadStatus<void>{};
}

} // namespace hogd
