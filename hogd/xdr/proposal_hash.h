#pragma once

#include <cstddef>

#include "hogd/base/log.h"
#include "hogd/crypto/crypto.h"
#include "hogd/crypto/domain_separators.h"
#include "parallel.hh"
#include "proposal.hh"
#include "x23/serialize.h"

namespace hogd {

// Shared with chuckd and the coordinator; changing it is a protocol change.
// The segment hashes are combined in order, not by XOR (hogd#4508).
inline SHA256Hash
hash_parallel_batch_compatibility(const ParallelBatch &batch)
{
  CHECK_EQ(batch.segment_tokens.size(), batch.segment_bytes.size());

  ParallelBatch index{
      .segment_tokens = batch.segment_tokens,
      .segment_bytes = batch.segment_bytes,
  };
  SHA256Hasher hasher;
  hasher.update(kParallelBatchDomainSeparatorCxs);
  hasher.update(sha256(index));

  size_t offset = 0;
  for (uint32_t segment_size : batch.segment_bytes) {
    CHECK_LE(offset, batch.xdr.size());
    CHECK_LE(segment_size, batch.xdr.size() - offset);

    x23::BufView segment;
    if (segment_size != 0) {
      segment = x23::BufView(batch.xdr.data() + offset, segment_size);
    }
    hasher.update(sha256(segment));
    offset += segment_size;
  }
  CHECK_EQ(offset, batch.xdr.size());
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
