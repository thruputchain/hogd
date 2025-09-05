#pragma once

#include "hogd/base/log.h"
#include "hogd/crypto/crypto.h"
#include "transaction.hh"
#include "x23/x23/serialize.h"

namespace hogd {

class Authentication {
public:
  bool authenticate(const SignedTransaction &tx)
  {
    const AuthSignaturePreimage preimage{.signature_weight = 0,
                                         .message = x23::serialize(tx.tx)};
    const x23::Buf msg = x23::serialize(preimage);
    // TODO(jhumphri): Remove comment.
    // [Ed25519PublicKey] is an [std::array<unsigned char, 32>].
    for (const DecoratedSignature &dec_sig : tx.signatures) {
      if (!check_decorated_signature(dec_sig, msg)) {
        return false;
      }
    }
    for (const AuxiliaryDecoratedSignatures &aux : tx.aux_signatures) {
      const ContractAddress &account = aux.address;
      for (const DecoratedSignature &dec_sig : aux.signatures) {
        if (!check_decorated_signature(dec_sig, msg)) {
          return false;
        }
      }
    }
    return true;
  }

private:
  bool check_decorated_signature(const DecoratedSignature &dec_sig,
                                 const x23::Buf &msg)
  {
    // TODO(jhumphri): Populate [public_key].
    const Ed25519PublicKey public_key = {};
    CHECK_EQ(dec_sig.signature.scheme(), CryptographicScheme::Ed25519);
    const Ed25519Signature sig = dec_sig.signature.ed25519();
    return ed25519_verify(public_key, sig, msg);
  }
};

} // namespace hogd
