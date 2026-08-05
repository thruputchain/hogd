// clang-format off

%#include "consensus.hh"
%#include "indexer.hh"
%#include "network.hh"
%#include "proposal.hh"
%#include "rpc_types.hh"
%#include "transaction.hh"
%#include "types.hh"

namespace hogd
{
// clang-format on

// Private durable event-to-indexer outbox schema. This is intentionally not a
// wire protocol: only hogd reads these files. The stable envelope is decoded
// first. Its protocol field gates decoding of payload structs that contain
// current protocol types. Old-protocol files therefore require a matching
// binary or an explicit migration; they must not be decoded after shape drift.
enum IndexerOutboxRecordKind {
  IndexerOutboxCandidate = 0,
  IndexerOutboxCommitted = 1,
  IndexerOutboxEpochOrigin = 2
};

struct IndexerOutboxEpochOrigin {
  unsigned int format_version;
  unsigned hyper epoch_sequence;
  IndexerOutboxOrigin origin;
};

union IndexerOutboxRecordIdentity switch (IndexerOutboxRecordKind kind) {
case IndexerOutboxCandidate:
  BlockID candidate_block_id;
default:
  void;
};

// This bounded header is followed directly by payload_byte_length raw bytes.
// Enumeration decodes only this header; payload XDR is protocol-gated. The
// discriminated identity encodes BlockID only for candidate records.
struct IndexerOutboxRecordHeader {
  unsigned int format_version;
  IndexerOutboxRecordIdentity record_identity;
  unsigned hyper epoch_sequence;
  BlockHeight block_height;
  ProtocolCommit protocol;
  unsigned hyper payload_byte_length;
  SHA256Hash payload_digest;
  ConsensusValueHash consensus_hash;
  ConsensusValueHash prev_hash;
  EventsHash event_root;
  ProposalTxsHash proposal_txs_hash;
};

struct IndexerOutboxCandidatePayload {
  unsigned int format_version;
  BlockID block_id;
  BlockHeight block_height;
  EventsHash event_root;
  ProposalTxs proposal_txs;
  TxEventLogBatch event_logs;
  ProposedTransactionBatch tx_bodies;
};

typedef IndexerCommittedBlockPayload IndexerOutboxCommittedPayload;

} // namespace hogd
