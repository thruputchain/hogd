#pragma once

#include <cstddef>

#include "hogd/base/log.h"
#include "hogd/crypto/crypto.h"
#include "hogd/crypto/domain_separators.h"
#include "parallel.hh"
#include "proposal.hh"
#include "x23/serialize.h"
#include "x23/serialize_cxs.h"

namespace hogd {

// The hash that commits to batch, shared with chuckd and the coordinator:
// SHA256(cxs(DomainSeparator("ParallelBatch")) || cxs(BatchCommit)).
inline SHA256Hash
hash_parallel_batch_compatibility(const ParallelBatch &batch)
{
  CHECK_EQ(batch.segment_tokens.size(), batch.segment_bytes.size());

  x23::Vec<SHA256Hash, 0xffffffff> segment_hashes;
  segment_hashes.reserve(batch.segment_bytes.size());
  size_t offset = 0;
  for (uint32_t segment_size : batch.segment_bytes) {
    CHECK_LE(offset, batch.xdr.size());
    CHECK_LE(segment_size, batch.xdr.size() - offset);

    x23::BufView segment;
    if (segment_size != 0) {
      segment = x23::BufView(batch.xdr.data() + offset, segment_size);
    }
    segment_hashes.push_back(sha256(segment));
    offset += segment_size;
  }
  CHECK_EQ(offset, batch.xdr.size());

  // The pinned protocol has no BatchCommit yet, so its fields are encoded in
  // order: an ext of 0, then the arrays. A count or length is at most 5 bytes.
  x23::Buf fields;
  fields.reserve(16 + (42 * segment_hashes.size()));
  CHECK(x23::serialize_cxs_to(fields, ExtensionPoint(0)) &&
        x23::serialize_cxs_to(fields, batch.segment_tokens) &&
        x23::serialize_cxs_to(fields, batch.segment_bytes) &&
        x23::serialize_cxs_to(fields, segment_hashes));
  SHA256Hasher hasher;
  hasher.update(kParallelBatchDomainSeparatorCxs);
  hasher.update(fields);
  return hasher.finalize();
}

// SHA256(cxs(DomainSeparator("ProposalTxs")) || cxs(ProposalTxsCommit)), with
// the ProposalTxsCommit of thruput-protocol#266.
inline ProposalTxsHash
proposal_txs_compatibility_hash(const ProposalTxs &txs)
{
  const SHA256Hash signed_hash =
      hash_parallel_batch_compatibility(txs.signed_hash);
  const SHA256Hash nondeterministic_data =
      hash_parallel_batch_compatibility(txs.nondeterministic_data);

  // The pinned protocol has no ProposalTxsCommit yet, so its fields are encoded
  // in order: the ext of txs, then the hashes of the two batches.
  x23::Buf fields;
  fields.reserve(65);
  CHECK(x23::serialize_cxs_to(fields, txs.ext) &&
        x23::serialize_cxs_to(fields, signed_hash) &&
        x23::serialize_cxs_to(fields, nondeterministic_data));
  SHA256Hasher hasher;
  hasher.update(kProposalTxsDomainSeparatorCxs);
  hasher.update(fields);
  return hasher.finalize();
}

} // namespace hogd
