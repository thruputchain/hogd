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
// SHA256(cxs(BatchCommit)), with the BatchCommit of thruput-protocol#266.
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

  // The pinned protocol has no BatchCommit yet, so the fields after its domain
  // separator are encoded in order. A count or a length takes at most 5 bytes.
  x23::Buf fields;
  fields.reserve(15 + (42 * segment_hashes.size()));
  CHECK(x23::serialize_cxs_to(fields, batch.segment_tokens) &&
        x23::serialize_cxs_to(fields, batch.segment_bytes) &&
        x23::serialize_cxs_to(fields, segment_hashes));
  SHA256Hasher hasher;
  hasher.update(kParallelBatchDomainSeparatorCxs);
  hasher.update(fields);
  return hasher.finalize();
}

// SHA256(cxs(ProposalTxsCommit)), with the ProposalTxsCommit of
// thruput-protocol#266: the domain separator, then the two batch hashes.
inline ProposalTxsHash
proposal_txs_compatibility_hash(const ProposalTxs &txs)
{
  // The pinned protocol has no ProposalTxsCommit yet. cxs writes a SHA256Hash
  // as its 32 bytes, so the fields go to the hasher one after another.
  SHA256Hasher hasher;
  hasher.update(kProposalTxsDomainSeparatorCxs);
  hasher.update(hash_parallel_batch_compatibility(txs.signed_hash));
  hasher.update(hash_parallel_batch_compatibility(txs.nondeterministic_data));
  return hasher.finalize();
}

} // namespace hogd
