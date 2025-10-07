#pragma once

#include "account.hh"
#include "db_sched.hh"
#include "hogd/base/log.h"
#include "hogd/crypto/crypto.h"
#include "hogd/mempool/types.h"
#include "rpc_types.hh"
#include "transaction.hh"
#include "types.hh"
#include "x23/vec.h"
#include "x23/x23/serialize.h"
#include <unordered_map>

namespace hogd {

// TODO(jhumphri): Handle [block_expiry_height].
class Authentication {
public:
  Authentication(BlockID current_block_id, db_sched_client_t &db_sched_client)
    : current_block_id_(current_block_id), db_sched_client_(db_sched_client)
  {}

  // Pulls in the newest [AccountAuthData] for all cached accounts. Returns any
  // accounts whose [AccountAuthData] has changed.
  x23::Async<x23::Vec<ContractAddress>> update_block_id(BlockID block_id)
  {
    current_block_id_ = block_id;

    x23::Vec<ContractAddress> accounts;
    accounts.reserve(accounts_.size());
    for (auto &[account, _] : accounts_) {
      accounts.push_back(account);
    }

    DbAccountAuthDataBatchResult res =
        co_await db_sched_client_.dbsched_get_account_auth_data(
            current_block_id_, accounts);
    CHECK_EQ(res.result(), DbSchedResult::DbSchedResultOK);
    CHECK_EQ(res.account_auth_data_batch().size(), accounts.size());

    x23::Vec<ContractAddress> ret;
    for (size_t i = 0; i < res.account_auth_data_batch().size(); i++) {
      const ContractAddress &account = accounts[i];
      // TODO(jhumphri): We only need to reauthenticate the txs for [account] if
      // the refreshed auth data differs from the auth data we already have.
      if (res.account_auth_data_batch()[i] != accounts_[account]) {
        accounts_[account] = res.account_auth_data_batch()[i];
        ret.push_back(account);
      }
    }
    co_return ret;
  }

  x23::Async<bool> authenticate(const SignedTransaction &tx)
  {
    AccountAuthData *auth_data =
        co_await get_account(current_block_id_,
                             tx.tx.invocation.invoked_address);
    if (!auth_data) {
      co_return false;
    }
    const AuthSignaturePreimage preimage{.signature_weight =
                                             tx.signature_weight,
                                         .message = x23::serialize(tx.tx)};
    const x23::Buf msg = x23::serialize(preimage);

    Weight total_weight = 0;
    Weight nonce_weight = 0;
    x23::Vec<Weight, MAX_GAS_CONFIGS> gas_weights(MAX_GAS_CONFIGS, 0);
    for (const DecoratedSignature &dec_sig : tx.signatures) {
      Weight weight = 0;
      Ed25519PublicKey public_key;
      if (!get_public_key(*auth_data, dec_sig.public_key_id, weight,
                          public_key)) {
        co_return false;
      }
      if (!check_decorated_signature(dec_sig, public_key, msg)) {
        co_return false;
      }
      if (dec_sig.public_key_id >= MAX_SIGNERS) {
        co_return false;
      }
      total_weight += weight;
      if ((1 << dec_sig.public_key_id) &
          auth_data->nonce_config.accepted_keys) {
        nonce_weight += weight;
      }
      for (size_t i = 0; i < auth_data->gas_permissions.size(); i++) {
        const GasPermissionConfig &gas_config = auth_data->gas_permissions[i];
        if ((1 << dec_sig.public_key_id) & gas_config.accepted_keys) {
          gas_weights[i] += weight;
        }
      }
    }
    // We check [nonce_weight] here rather than below because only the account
    // that submitted the tx can contribute its key weights to the nonce weight.
    // Auxiliary accounts cannot do this.
    if (nonce_weight < auth_data->nonce_config.required_weight) {
      co_return false;
    }
    // Similarly, we check if we have enough gas available here rather than
    // below because only the account that submitted the tx can contribute gas.
    // TODO(jhumphri): Support donated gas from other accounts.
    GasAmount gas_available = 0;
    for (size_t i = 0; i < auth_data->gas_permissions.size(); i++) {
      const GasPermissionConfig &gas_config = auth_data->gas_permissions[i];
      if (gas_weights[i] >= gas_config.required_weight) {
        gas_available += gas_config.limit;
      }
    }
    if (gas_available < tx.tx.invocation.gas_limit) {
      co_return false;
    }

    for (const AuxiliaryDecoratedSignatures &aux : tx.aux_signatures) {
      AccountAuthData *aux_auth_data =
          co_await get_account(current_block_id_, aux.address);
      if (!aux_auth_data) {
        co_return false;
      }

      for (const DecoratedSignature &dec_sig : aux.signatures) {
        Weight weight = 0;
        Ed25519PublicKey public_key;
        if (!get_public_key(*aux_auth_data, dec_sig.public_key_id, weight,
                            public_key)) {
          co_return false;
        }
        if (!check_decorated_signature(dec_sig, public_key, msg)) {
          co_return false;
        }
        total_weight += weight;
      }
    }
    // We check [total_weight] down here rather than above with the nonce weight
    // because we need to include the weight of the auxiliary keys.
    co_return total_weight >= tx.signature_weight;
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
  static bool get_public_key(const AccountAuthData &auth_data, size_t idx,
                             Weight &max_weight, Ed25519PublicKey &public_key)
  {
    if (idx >= auth_data.public_keys.size()) {
      return false;
    }
    const auto &pk_config = auth_data.public_keys[idx];
    if (!pk_config) {
      return false;
    }
    if (pk_config->key.scheme() != CryptographicScheme::Ed25519) {
      return false;
    }
    max_weight = pk_config->max_weight;
    public_key = pk_config->key.ed25519();
    return true;
  }

  x23::Async<AccountAuthData *> get_account(BlockID block_id,
                                            const ContractAddress &account)
  {
    auto it = accounts_.find(account);
    if (it != accounts_.end()) {
      return &it->second;
    }

    DbAccountAuthDataBatchResult res =
        co_await db_sched_client_.dbsched_get_account_auth_data(block_id,
                                                                {account});
    if (res.result() != DbSchedResult::DbSchedResultOK) {
      co_return nullptr;
    }
    CHECK_EQ(res.account_auth_data_batch().size(), 1UL);
    accounts_[account] = res.account_auth_data_batch()[0];
    co_return &accounts_[account];
  }

  BlockID current_block_id_;
  db_sched_client_t &db_sched_client_;
  std::unordered_map<ContractAddress, AccountAuthData> accounts_;
};

} // namespace hogd
