#pragma once

#include "hogd/base/log.h"
#include "hogd/crypto/crypto.h"
#include "hogd/mempool/types.h"
#include "transaction.hh"
#include "x23/x23/serialize.h"

namespace hogd {

class Authentication {
public:
  Authentication(db_client_t &db_client) : db_client_(db_client) {}

  // TODO(jhumphri): Remove this.
  std::pair<Ed25519PublicKey, Ed25519SecretKey> get_tmp_key_pair()
  {
    static const std::pair<Ed25519PublicKey, Ed25519SecretKey> keys =
        ed25519_keygen();
    return keys;
  }

  bool authenticate(const SignedTransaction &tx)
  {
    // TODO(jhumphri): Populate [public_key].
    const Ed25519PublicKey public_key = get_tmp_key_pair().first;

    const AuthSignaturePreimage preimage{.signature_weight = 0,
                                         .message = x23::serialize(tx.tx)};
    const x23::Buf msg = x23::serialize(preimage);
    for (const DecoratedSignature &dec_sig : tx.signatures) {
      if (!check_decorated_signature(dec_sig, public_key, msg)) {
        return false;
      }
    }
    for (const AuxiliaryDecoratedSignatures &aux : tx.aux_signatures) {
      const ContractAddress &account = aux.address;
      for (const DecoratedSignature &dec_sig : aux.signatures) {
        if (!check_decorated_signature(dec_sig, public_key, msg)) {
          return false;
        }
      }
    }
    return true;
  }

private:
  bool check_decorated_signature(const DecoratedSignature &dec_sig,
                                 const Ed25519PublicKey &public_key,
                                 const x23::Buf &msg)
  {
    CHECK_EQ(dec_sig.signature.scheme(), CryptographicScheme::Ed25519);
    const Ed25519Signature sig = dec_sig.signature.ed25519();
    return ed25519_verify(public_key, sig, msg);
  }

  db_client_t &db_client_;
};

} // namespace hogd
