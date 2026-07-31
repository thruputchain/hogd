#include "hogd/xdr/indexer_payload_validation.h"

#include <cstddef>
#include <expected>
#include <format>
#include <span>
#include <unordered_map>
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

IndexerPayloadStatus<ProposalTxsHash>
validate_canonical_proposal_mapping(const IndexerCommittedBlockPayload &payload)
{
  x23::XdrVec<SignedTransactionHash> canonical_hashes;
  x23::XdrVec<NondeterministicProposalData> canonical_nondeterministic_data;
  if (!parallel_batch_to_t_serial(payload.proposal_txs.hash,
                                  canonical_hashes) ||
      !parallel_batch_to_t_serial(payload.proposal_txs.nondeterministic_data,
                                  canonical_nondeterministic_data)) {
    return std::unexpected("malformed canonical ProposalTxs parallel batch");
  }
  if (canonical_hashes.size() != canonical_nondeterministic_data.size()) {
    return std::unexpected("canonical ProposalTxs batch cardinalities differ");
  }
  if (canonical_hashes.size() != payload.event_logs.size() ||
      canonical_hashes.size() != payload.tx_bodies.size()) {
    return std::unexpected(
        "canonical proposal and event/body batch cardinalities differ");
  }

  std::unordered_map<SignedTransactionHash, NondeterministicProposalData,
                     ByteArrayHasher>
      canonical;
  canonical.reserve(canonical_hashes.size());
  for (size_t i = 0; i < canonical_hashes.size(); ++i) {
    if (!canonical
             .emplace(canonical_hashes[i], canonical_nondeterministic_data[i])
             .second) {
      return std::unexpected("canonical ProposalTxs contains duplicate hashes");
    }
  }

  std::unordered_set<SignedTransactionHash, ByteArrayHasher> seen_bodies;
  seen_bodies.reserve(payload.tx_bodies.size());
  for (size_t i = 0; i < payload.tx_bodies.size(); ++i) {
    const SignedTransactionHash body_hash =
        sha256(payload.tx_bodies[i].signed_tx.tx);
    if (body_hash != payload.event_logs[i].proposed_tx_hash) {
      return std::unexpected(std::format(
          "transaction body hash differs from event log at index {}", i));
    }
    if (!seen_bodies.insert(body_hash).second) {
      return std::unexpected("event/body batches contain duplicate hashes");
    }
    auto entry = canonical.find(body_hash);
    if (entry == canonical.end()) {
      return std::unexpected(
          "event/body transaction is missing from canonical ProposalTxs");
    }
    if (!nondeterministic_data_equal(
            payload.tx_bodies[i].nondeterministic_proposal_data,
            entry->second)) {
      return std::unexpected(
          "transaction body nondeterministic data differs from canonical "
          "ProposalTxs");
    }
  }
  if (seen_bodies.size() != canonical.size()) {
    return std::unexpected("canonical ProposalTxs contains extra transactions");
  }
  return proposal_txs_compatibility_hash(payload.proposal_txs);
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

IndexerPayloadStatus<TxsHash>
compute_indexer_proposal_txs_root(const ProposalTxs &proposal_txs)
{
  x23::XdrVec<SignedTransactionHash> hashes;
  if (!parallel_batch_to_t_serial(proposal_txs.hash, hashes)) {
    return std::unexpected("malformed canonical transaction hash batch");
  }
  MerkleTriePrefix trie(/*allocator_type=*/Allocator<Node>::Type::kBump);
  for (const SignedTransactionHash &hash : hashes) {
    if (!trie.insert(HexArray(hash), std::span<const std::byte>())) {
      return std::unexpected("canonical transaction hash batch has duplicates");
    }
  }
  trie.generate_all_hashes(/*num_threads=*/1);
  return trie.get_root_hash();
}

IndexerPayloadStatus<void>
validate_indexer_committed_payload(const IndexerCommittedBlockPayload &payload)
{
  if (payload.format_version != kIndexerOutboxFormatVersion) {
    return std::unexpected(
        std::format("unknown committed outbox format version {}",
                    payload.format_version));
  }
  if (payload.consensus_value.proposal_header.block_height < 0) {
    return std::unexpected("committed outbox height is negative");
  }
  if (payload.event_logs.size() != payload.tx_bodies.size()) {
    return std::unexpected("event log and transaction body batch sizes differ");
  }
  if (compute_indexer_outbox_event_root(payload.event_logs) !=
      payload.consensus_value.proposal_header.events_hash) {
    return std::unexpected("event root differs from payload identity");
  }
  auto proposal_hash = validate_canonical_proposal_mapping(payload);
  if (!proposal_hash) {
    return std::unexpected(proposal_hash.error());
  }
  if (*proposal_hash != payload.consensus_value.proposal_txs_hash) {
    return std::unexpected(
        "canonical ProposalTxs commitment differs from ConsensusValue");
  }
  auto txs_root = compute_indexer_proposal_txs_root(payload.proposal_txs);
  if (!txs_root) {
    return std::unexpected(txs_root.error());
  }
  if (*txs_root != payload.consensus_value.proposal_header.txs_hash) {
    return std::unexpected(
        "canonical transaction root differs from ProposalHeader");
  }
  return {};
}

IndexerPayloadStatus<IndexerCommittedBlock>
make_indexer_committed_block(const IndexerCommittedBlockPayload &payload)
{
  if (auto valid = validate_indexer_committed_payload(payload); !valid) {
    return std::unexpected(valid.error());
  }
  return IndexerCommittedBlock{
      .payload = payload,
      .identity =
          IndexerCommittedBlockIdentity{
              .consensus_hash = sha256(payload.consensus_value),
              .payload_digest = sha256(payload),
          },
  };
}

IndexerPayloadStatus<void>
validate_indexer_committed_block(const IndexerCommittedBlock &block)
{
  if (auto valid = validate_indexer_committed_payload(block.payload); !valid) {
    return valid;
  }
  if (block.identity.consensus_hash != sha256(block.payload.consensus_value) ||
      sha256(block.payload) != block.identity.payload_digest) {
    return std::unexpected(
        "committed block identity differs from canonical payload");
  }
  return {};
}

} // namespace hogd
