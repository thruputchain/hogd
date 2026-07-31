#pragma once

#include <cstddef>

#include "hogd/base/log.h"
#include "hogd/crypto/crypto.h"
#include "parallel.hh"
#include "proposal.hh"
#include "x23/serialize.h"

namespace hogd {

// Compatibility behavior for chuckd's proposal hash. This is intentionally not
// SHA-256 of canonical ProposalTxs XDR; changing it requires a protocol
// migration.
inline SHA256Hash
hash_parallel_batch_compatibility(const ParallelBatch &batch)
{
  CHECK_EQ(batch.segment_tokens.size(), batch.segment_bytes.size());

  SHA256Hash result{};
  size_t offset = 0;
  for (uint32_t segment_size : batch.segment_bytes) {
    CHECK_LE(offset, batch.xdr.size());
    CHECK_LE(segment_size, batch.xdr.size() - offset);

    x23::BufView segment;
    if (segment_size != 0) {
      segment = x23::BufView(batch.xdr.data() + offset, segment_size);
    }
    SHA256Hash segment_hash = sha256(segment);
    for (size_t i = 0; i < result.size(); ++i) {
      result[i] ^= segment_hash[i];
    }
    offset += segment_size;
  }
  CHECK_EQ(offset, batch.xdr.size());

  ParallelBatch index = batch;
  index.xdr.clear();
  SHA256Hash index_hash = sha256(index);
  for (size_t i = 0; i < result.size(); ++i) {
    result[i] ^= index_hash[i];
  }
  return result;
}

inline ProposalTxsHash
proposal_txs_compatibility_hash(const ProposalTxs &txs)
{
  ProposalTxs base = txs;
  base.hash = {};
  base.nondeterministic_data = {};

  ProposalTxsHash result = sha256(base);
  const SHA256Hash hash = hash_parallel_batch_compatibility(txs.hash);
  const SHA256Hash nondeterministic_data_hash =
      hash_parallel_batch_compatibility(txs.nondeterministic_data);
  for (size_t i = 0; i < result.size(); ++i) {
    result[i] ^= hash[i] ^ nondeterministic_data_hash[i];
  }
  return result;
}

} // namespace hogd
