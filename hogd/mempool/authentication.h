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
  void add_account(const ContractAddress &account, const AccountAuthData &data)
  {
    CHECK(!accounts_.contains(account));
    accounts_[account] = data;
  }

  bool authenticate(const SignedTransaction &tx)
  {
    CHECK(accounts_.contains(tx.tx.invocation.invoked_address));
    const AccountAuthData &auth_data =
        accounts_[tx.tx.invocation.invoked_address];

    const AuthSignaturePreimage preimage{.signature_weight =
                                             tx.signature_weight,
                                         .message = x23::serialize(tx.tx)};
    const x23::Buf msg = x23::serialize(preimage);
    Weight total_weight = 0;
    Weight nonce_weight = 0;
    for (const DecoratedSignature &dec_sig : tx.signatures) {
      Weight weight = 0;
      Ed25519PublicKey public_key;
      if (!get_public_key(tx.tx.invocation.invoked_address,
                          dec_sig.public_key_id, weight, public_key)) {
        return false;
      }
      if (!check_decorated_signature(dec_sig, public_key, msg)) {
        return false;
      }
      if (dec_sig.public_key_id >= MAX_SIGNERS) {
        return false;
      }
      total_weight += weight;
      if ((1 << dec_sig.public_key_id) & auth_data.nonce_config.accepted_keys) {
        nonce_weight += weight;
      }
    }
    // We check [nonce_weight] here rather than below because only the account
    // that submitted the tx can contribute its key weights to the nonce weight.
    // Auxiliary accounts cannot do this.
    if (nonce_weight < auth_data.nonce_config.required_weight) {
      return false;
    }

    for (const AuxiliaryDecoratedSignatures &aux : tx.aux_signatures) {
      const ContractAddress &account = aux.address;
      for (const DecoratedSignature &dec_sig : aux.signatures) {
        Weight weight = 0;
        Ed25519PublicKey public_key;
        if (!get_public_key(account, dec_sig.public_key_id, weight,
                            public_key)) {
          return false;
        }
        if (!check_decorated_signature(dec_sig, public_key, msg)) {
          return false;
        }
        total_weight += weight;
      }
    }
    // We check [total_weight] down here rather than above with the nonce weight
    // because we need to include the weight of the auxiliary keys.
    return total_weight >= tx.signature_weight;
  }

  static SignedTransaction
  sign(const Transaction &tx, Weight signature_weight,
       const std::unordered_map<uint32_t, Ed25519SecretKey>
           &account_private_keys,
       const std::unordered_map<ContractAddress,
                                std::unordered_map<uint32_t, Ed25519SecretKey>>
           &auxiliary_private_keys)
  {
    const AuthSignaturePreimage preimage{.signature_weight = signature_weight,
                                         .message = x23::serialize(tx)};
    const x23::Buf msg = x23::serialize(preimage);
    SignedTransaction signed_tx{.tx = tx,
                                .signature_weight = signature_weight,
                                .signatures = {},
                                .aux_signatures = {}};
    signed_tx.signatures.reserve(account_private_keys.size());
    for (const auto &[idx, private_key] : account_private_keys) {
      Signature sig(CryptographicScheme::Ed25519);
      sig.ed25519() = ed25519_sign(private_key, msg);
      const DecoratedSignature dec_sig = {.public_key_id = idx,
                                          .signature = sig};
      signed_tx.signatures.push_back(dec_sig);
    }

    std::unordered_map<ContractAddress, x23::Vec<DecoratedSignature>> aux_map;
    for (const auto &[account, private_keys] : auxiliary_private_keys) {
      for (const auto &[idx, private_key] : private_keys) {
        Signature sig(CryptographicScheme::Ed25519);
        sig.ed25519() = ed25519_sign(private_key, msg);
        const DecoratedSignature dec_sig = {.public_key_id = idx,
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
                      Weight &max_weight, Ed25519PublicKey &public_key)
  {
    if (!accounts_.contains(account)) {
      return false;
    }
    if (idx >= accounts_[account].public_keys.size()) {
      return false;
    }
    const PublicKeyConfig &pk_config = accounts_[account].public_keys[idx];
    if (!pk_config.valid) {
      return false;
    }
    if (pk_config.key.scheme() != CryptographicScheme::Ed25519) {
      return false;
    }
    max_weight = pk_config.max_weight;
    public_key = pk_config.key.ed25519();
    return true;
  }

  std::unordered_map<ContractAddress, AccountAuthData> accounts_;
};

} // namespace hogd
