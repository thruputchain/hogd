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
// SHA256(cxs(DomainSeparator("ParallelBatch")) || cxs(segment_tokens) ||
// cxs(segment_bytes) || SHA256(SHA256(segment_0) || SHA256(segment_1) || ...)).
inline SHA256Hash
hash_parallel_batch_compatibility(const ParallelBatch &batch)
{
  CHECK_EQ(batch.segment_tokens.size(), batch.segment_bytes.size());

  SHA256Hasher segment_hashes_hasher;
  size_t offset = 0;
  for (uint32_t segment_size : batch.segment_bytes) {
    CHECK_LE(offset, batch.xdr.size());
    CHECK_LE(segment_size, batch.xdr.size() - offset);

    x23::BufView segment;
    if (segment_size != 0) {
      segment = x23::BufView(batch.xdr.data() + offset, segment_size);
    }
    segment_hashes_hasher.update(sha256(segment));
    offset += segment_size;
  }
  CHECK_EQ(offset, batch.xdr.size());
  const SHA256Hash segment_hashes_hash = segment_hashes_hasher.finalize();

  // In cxs the three fields encode back to back, the hash as its 32 bytes.
  // A count takes at most 5 bytes, so one allocation holds the preimage.
  x23::Buf preimage;
  preimage.reserve(10 * (1 + batch.segment_bytes.size()) +
                   segment_hashes_hash.size());
  CHECK(x23::serialize_cxs_to(preimage, batch.segment_tokens) &&
        x23::serialize_cxs_to(preimage, batch.segment_bytes) &&
        x23::serialize_cxs_to(preimage, segment_hashes_hash));
  SHA256Hasher hasher;
  hasher.update(kParallelBatchDomainSeparatorCxs);
  hasher.update(preimage);
  return hasher.finalize();
}

// Ordered SHA-256 over the base (txs with empty batches) and both batch
// hashes, so the batches cannot trade places (hogd#4508).
inline ProposalTxsHash
proposal_txs_compatibility_hash(const ProposalTxs &txs)
{
  ProposalTxs base{.ext = txs.ext};

  SHA256Hasher hasher;
  hasher.update(kProposalTxsDomainSeparatorCxs);
  hasher.update(sha256(base));
  hasher.update(hash_parallel_batch_compatibility(txs.signed_hash));
  hasher.update(hash_parallel_batch_compatibility(txs.nondeterministic_data));
  return hasher.finalize();
}

} // namespace hogd
