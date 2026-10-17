#pragma once

#include <cstddef>

#include "hogd/base/log.h"
#include "hogd/crypto/crypto.h"
#include "transaction.hh"
#include "types.hh"
#include "x23/serialize.h"
#include "x23/vec.h"

namespace hogd {

// Identifies one exact signed encoding of a transaction: SHA-256 of the
// canonical XDR of its [SignedTransaction], signatures and pubkey claim key
// included. A [TransactionHash] covers only the [Transaction] the signatures
// are over, so two signings of one body share a [TransactionHash] but not a
// [SignedTransactionHash].
using SignedTransactionHash = SHA256Hash;
using SignedTransactionHashBatch = x23::XdrVec<SignedTransactionHash>;

struct SignedTransactionHashes {
  TransactionHash tx_hash;
  SignedTransactionHash signed_tx_hash;
};

// Both hashes of [stx] from one serialization. The XDR of a
// [SignedTransaction] begins with the XDR of its [Transaction], so the body
// hash is the SHA-256 state at that boundary, finalized. The input is
// re-encoded rather than taken as received, so the signed hash is canonical.
inline SignedTransactionHashes
hash_signed_transaction(const SignedTransaction &stx)
{
  const x23::Buf bytes = x23::serialize(stx);
  const size_t body_size = x23::xdr_size(stx.tx);
  CHECK_LE(body_size, bytes.size());

  SHA256Hasher hasher;
  hasher.update(x23::BufView(bytes.data(), body_size));
  SHA256Hasher body_hasher = hasher;
  hasher.update(
      x23::BufView(bytes.data() + body_size, bytes.size() - body_size));
  return {.tx_hash = body_hasher.finalize(),
          .signed_tx_hash = hasher.finalize()};
}

} // namespace hogd
