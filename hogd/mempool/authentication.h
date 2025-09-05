#pragma once

#include "transaction.hh"
#include "x23/x23/serialize.h"

namespace hogd {

class Authentication {
public:
  bool authenticate(const SignedTransaction &tx)
  {
    const AuthSignaturePreimage preimage{.signature_weight = 0,
                                         .message = x23::serialize(tx.tx)};
    const x23::Buf to_sign = x23::serialize(preimage);
    for (const DecoratedSignature &sig : tx.signatures) {
      // TODO(jhumphri): Verify [sig].
    }
    for (const AuxiliaryDecoratedSignatures &aux : tx.aux_signatures) {
      const ContractAddress &account = aux.address;
      for (const DecoratedSignature &sig : aux.signatures) {
        // TODO(jhumphri): Verify [sig].
      }
    }
    return false;
  }
};

} // namespace hogd
