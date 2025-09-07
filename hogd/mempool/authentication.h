#pragma once

#include "account.hh"
#include "hogd/base/log.h"
#include "hogd/crypto/crypto.h"
#include "hogd/mempool/types.h"
#include "transaction.hh"
#include "x23/x23/serialize.h"

namespace hogd {

class Authentication {
public:
  Authentication() = default;

  // TODO(jhumphri): Remove this.
  Ed25519SecretKey create_tmp_key_pair(const ContractAddress &account,
                                       size_t idx)
  {
    const auto &[public_key, private_key] = ed25519_keygen();
    public_keys_[account][idx] = public_key;
    return private_key;
  }

  // TODO(jhumphri): Remove this.
  void add_account(const ContractAddress &account, const AccountAuthData &data) {
    accounts_[account] = data;
  }

  bool authenticate(const SignedTransaction &tx)
  {
    const AuthSignaturePreimage preimage{.signature_weight = 0,
                                         .message = x23::serialize(tx.tx)};
    const x23::Buf msg = x23::serialize(preimage);
    for (const DecoratedSignature &dec_sig : tx.signatures) {
      Ed25519PublicKey public_key;
      if (!get_public_key(tx.tx.invocation.invoked_address,
                          dec_sig.public_key_id, public_key)) {
        return false;
      }
      if (!check_decorated_signature(dec_sig, public_key, msg)) {
        return false;
      }
    }
    for (const AuxiliaryDecoratedSignatures &aux : tx.aux_signatures) {
      const ContractAddress &account = aux.address;
      for (const DecoratedSignature &dec_sig : aux.signatures) {
        Ed25519PublicKey public_key;
        if (!get_public_key(account, dec_sig.public_key_id, public_key)) {
          return false;
        }
        if (!check_decorated_signature(dec_sig, public_key, msg)) {
          return false;
        }
      }
    }
    return true;
  }

  static SignedTransaction
  sign(const Transaction &tx,
       const std::unordered_map<uint32_t, Ed25519SecretKey>
           &account_private_keys,
       const std::unordered_map<ContractAddress,
                                std::unordered_map<uint32_t, Ed25519SecretKey>>
           &auxiliary_private_keys)
  {
    const AuthSignaturePreimage preimage{.signature_weight = 0,
                                         .message = x23::serialize(tx)};
    const x23::Buf msg = x23::serialize(preimage);
    SignedTransaction signed_tx{.tx = tx,
                                .signatures = {},
                                .aux_signatures = {}};
    signed_tx.signatures.reserve(account_private_keys.size());
    for (const auto &[idx, private_key] : account_private_keys) {
      Signature sig(CryptographicScheme::Ed25519);
      sig.ed25519() = ed25519_sign(private_key, msg);
      const DecoratedSignature dec_sig = {.public_key_id = idx,
                                          .weight = 0,
                                          .signature = sig};
      signed_tx.signatures.push_back(dec_sig);
    }

    std::unordered_map<ContractAddress, x23::Vec<DecoratedSignature>> aux_map;
    for (const auto &[account, private_keys] : auxiliary_private_keys) {
      for (const auto &[idx, private_key] : private_keys) {
        Signature sig(CryptographicScheme::Ed25519);
        sig.ed25519() = ed25519_sign(private_key, msg);
        const DecoratedSignature dec_sig = {.public_key_id = idx,
                                            .weight = 0,
                                            .signature = sig};
        aux_map[account].push_back(dec_sig);
      }
    }
    signed_tx.aux_signatures.reserve(auxiliary_private_keys.size());
    for (const auto &[account, sigs] : aux_map) {
      const AuxiliaryDecoratedSignatures aux_sigs{.address = account,
                                                  .signatures = sigs};
      signed_tx.aux_signatures.push_back(aux_sigs);
    }
    return signed_tx;
  }

private:
  static bool check_decorated_signature(const DecoratedSignature &dec_sig,
                                        const Ed25519PublicKey &public_key,
                                        const x23::Buf &msg)
  {
    CHECK_EQ(dec_sig.signature.scheme(), CryptographicScheme::Ed25519);
    const Ed25519Signature sig = dec_sig.signature.ed25519();
    return ed25519_verify(public_key, sig, msg);
  }

  // TODO(jhumphri): Populate [public_key] properly.
  bool get_public_key(const ContractAddress &account, size_t idx,
                      Ed25519PublicKey &public_key)
  {
    if (!public_keys_.contains(account)) {
      return false;
    }
    if (!public_keys_[account].contains(idx)) {
      return false;
    }
    public_key = public_keys_[account][idx];
    return true;
  }

  std::unordered_map<ContractAddress, AccountAuthData> accounts_;
  std::unordered_map<ContractAddress,
                     std::unordered_map<size_t, Ed25519PublicKey>>
      public_keys_;
};

} // namespace hogd
