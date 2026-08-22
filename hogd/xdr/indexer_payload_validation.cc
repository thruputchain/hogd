#include "hogd/xdr/indexer_payload_validation.h"

#include <cstddef>
#include <expected>
#include <format>
#include <span>

#include "event.hh"
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
size_t
serialize_event_commitment_leaf(
    x23::Buf &buf, const TxEventLog &event,
    const NondeterministicProposalData &nondeterministic_data)
{
  buf.resize_uninitialized(0);
  x23::serialize_to(&buf, event);
  const size_t event_xdr_size = buf.size();
  x23::serialize_to(&buf, nondeterministic_data);
  return event_xdr_size;
}

IndexerPayloadStatus<IndexerPayloadRoots>
compute_indexer_payload_roots(const TxEventLogBatch &event_logs,
                              const ProposedTransactionBatch &tx_bodies)
{
  if (event_logs.size() != tx_bodies.size()) {
    return std::unexpected("event/body batch cardinalities differ");
  }

  MerkleTriePrefix event_trie(/*allocator_type=*/Allocator<Node>::Type::kBump);
  MerkleTriePrefix tx_trie(/*allocator_type=*/Allocator<Node>::Type::kBump);
  x23::Buf event_leaf;
  for (size_t i = 0; i < tx_bodies.size(); ++i) {
    const TransactionHash body_hash = sha256(tx_bodies[i].signed_tx.tx);
    if (body_hash != event_logs[i].tx_hash) {
      return std::unexpected(std::format(
          "transaction body hash differs from event log at index {}", i));
    }
    if (!tx_trie.insert(HexArray(body_hash), std::span<const std::byte>())) {
      return std::unexpected("event/body batch contains duplicate hashes");
    }
    serialize_event_commitment_leaf(
        event_leaf, event_logs[i], tx_bodies[i].nondeterministic_proposal_data);
    const std::span<const std::byte> bytes =
        std::as_bytes(std::span(event_leaf));
    if (!event_trie.insert(bytes.first<32>(), bytes.subspan(32))) {
      return std::unexpected("event/body batch contains duplicate hashes");
    }
  }
  event_trie.generate_all_hashes(/*num_threads=*/1);
  tx_trie.generate_all_hashes(/*num_threads=*/1);
  return IndexerPayloadRoots{.events_hash = event_trie.get_root_hash(),
                             .txs_hash = tx_trie.get_root_hash()};
}

EventsHash
compute_indexer_outbox_event_root(const TxEventLogBatch &event_logs,
                                  const ProposedTransactionBatch &tx_bodies)
{
  CHECK_EQ(event_logs.size(), tx_bodies.size());
  MerkleTriePrefix trie(/*allocator_type=*/Allocator<Node>::Type::kBump);
  x23::Buf event_leaf;
  for (size_t i = 0; i < event_logs.size(); ++i) {
    serialize_event_commitment_leaf(
        event_leaf, event_logs[i], tx_bodies[i].nondeterministic_proposal_data);
    const std::span<const std::byte> bytes =
        std::as_bytes(std::span(event_leaf));
    CHECK(trie.insert(bytes.first<32>(), bytes.subspan(32)));
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
  auto roots =
      compute_indexer_payload_roots(payload.event_logs, payload.tx_bodies);
  if (!roots) {
    co_return std::unexpected(roots.error());
  }
  if (roots->events_hash !=
      payload.consensus_value.proposal_header.events_hash) {
    co_return std::unexpected("event root differs from ProposalHeader");
  }
  if (roots->txs_hash != payload.consensus_value.proposal_header.txs_hash) {
    co_return std::unexpected("transaction root differs from ProposalHeader");
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
  if (block.identity.consensus_hash != sha256(block.payload.consensus_value)) {
    co_return std::unexpected(
        "committed block identity differs from canonical payload");
  }
  co_return IndexerPayloadStatus<void>{};
}

} // namespace hogd
