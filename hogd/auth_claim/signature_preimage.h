#pragma once

#include "hogd/base/xdr_util.h" // IWYU pragma: keep
#include "hogd/crypto/domain_separators.h"
#include "transaction.hh"
#include "types.hh"
#include "x23/serialize.h"
#include "x23/vec.h"

namespace hogd {

// The message every transaction signature is computed over:
//
//   cxs(DomainSeparator{"Transaction"}) || network_hash || xdr(tx)
//
// The domain separator keeps a transaction signature from being reinterpreted
// as a signature over some other object whose encoding a transaction could be
// made to imitate. The network hash binds the signature to a single network,
// so a transaction signed for one chain cannot be replayed on another that
// happens to carry the same address with the same auth data.
//
// Signers and verifiers must both derive the preimage here: any disagreement
// between the two rejects every signature on the network.
inline x23::Buf
transaction_signature_preimage(const NetworkHash &network_hash,
                               const Transaction &tx)
{
  x23::Buf preimage;
  preimage.reserve(kTransactionSignatureDomainSeparatorCxs.size() +
                   network_hash.size() + x23::xdr_size(tx));
  preimage.append(kTransactionSignatureDomainSeparatorCxs.begin(),
                  kTransactionSignatureDomainSeparatorCxs.end());
  preimage.append(network_hash.begin(), network_hash.end());
  x23::serialize_to(&preimage, tx);
  return preimage;
}

} // namespace hogd
