#include "hogd/xdr/indexer_payload_validation.h"

#include <cstddef>
#include <expected>
#include <format>
#include <span>
#include <unordered_set>

#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h"
#include "hogd/container/merkle/allocator.h"
#include "hogd/container/merkle/hex_array.h"
#include "hogd/container/merkle/merkle_trie.h"
#include "hogd/container/merkle/node.h"
#include "hogd/crypto/crypto.h"
#include "hogd/xdr/proposal_hash.h"
#include "indexer.hh"
#include "proposal.hh"
#include "transaction.hh"
#include "types.hh"
#include "x23/async.h"
#include "x23/serialize.h"
#include "x23/vec.h"

namespace hogd {
namespace {

bool
nondeterministic_data_equal(const NondeterministicProposalData &lhs,
                            const NondeterministicProposalData &rhs)
{
  return x23::serialize(lhs) == x23::serialize(rhs);
}

} // namespace

x23::Async<IndexerPayloadStatus<ProposalTxsHash>>
validate_indexer_canonical_proposal_mapping(
    const ProposalTxs &proposal_txs, const TxEventLogBatch &event_logs,
    const ProposedTransactionBatch &tx_bodies)
{
  x23::XdrVec<TransactionHash> canonical_hashes;
  x23::XdrVec<NondeterministicProposalData> canonical_nondeterministic_data;
  if (!(co_await parallel_batch_to_t(proposal_txs.hash, canonical_hashes)) ||
      !(co_await parallel_batch_to_t(proposal_txs.nondeterministic_data,
                                     canonical_nondeterministic_data))) {
    co_return std::unexpected("malformed canonical ProposalTxs parallel batch");
  }
  if (canonical_hashes.size() != canonical_nondeterministic_data.size()) {
    co_return std::unexpected(
        "canonical ProposalTxs batch cardinalities differ");
  }
  if (canonical_hashes.size() != event_logs.size() ||
      canonical_hashes.size() != tx_bodies.size()) {
    co_return std::unexpected(
        "canonical proposal and event/body batch cardinalities differ");
  }

  std::unordered_set<TransactionHash, SipHash24Hasher> canonical;
  canonical.reserve(canonical_hashes.size());
  for (const TransactionHash &hash : canonical_hashes) {
    if (!canonical.insert(hash).second) {
      co_return std::unexpected(
          "canonical ProposalTxs contains duplicate hashes");
    }
  }

  for (size_t i = 0; i < tx_bodies.size(); ++i) {
    const TransactionHash body_hash = sha256(tx_bodies[i].signed_tx.tx);
    if (body_hash != event_logs[i].tx_hash) {
      co_return std::unexpected(std::format(
          "transaction body hash differs from event log at index {}", i));
    }
    if (body_hash != canonical_hashes[i]) {
      co_return std::unexpected(
          "event/body order differs from canonical ProposalTxs");
    }
    if (!nondeterministic_data_equal(
            tx_bodies[i].nondeterministic_proposal_data,
            canonical_nondeterministic_data[i])) {
      co_return std::unexpected(
          "transaction body nondeterministic data differs from canonical "
          "ProposalTxs");
    }
  }
  co_return proposal_txs_compatibility_hash(proposal_txs);
}

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
  auto proposal_hash =
      co_await validate_indexer_canonical_proposal_mapping(payload.proposal_txs,
                                                           payload.event_logs,
                                                           payload.tx_bodies);
  if (!proposal_hash) {
    co_return std::unexpected(proposal_hash.error());
  }
  if (*proposal_hash != payload.consensus_value.proposal_txs_hash) {
    co_return std::unexpected(
        "canonical ProposalTxs commitment differs from ConsensusValue");
  }
  auto txs_root =
      co_await compute_indexer_proposal_txs_root(payload.proposal_txs);
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
