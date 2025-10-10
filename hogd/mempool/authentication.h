#pragma once

#include "account.hh"
#include "auth_claim.hh"
#include "db_sched.hh"
#include "hogd/auth_claim/verify.h"
#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "hogd/crypto/crypto.h"
#include "hogd/mempool/types.h"
#include "rpc_types.hh"
#include "transaction.hh"
#include "types.hh"
#include "x23/async.h"
#include "x23/vec.h"
#include <cassert>
#include <memory>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

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
      assert(accounts_.contains(account));
      if (res.account_auth_data_batch()[i] != *accounts_[account]) {
        accounts_[account] =
            std::make_unique<AccountAuthData>(res.account_auth_data_batch()[i]);
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

    std::unordered_set<ContractAddress> aux_contracts =
        get_aux_addresses(tx.tx);
    std::unordered_map<ContractAddress, const AccountAuthData *> aux_auth_map;
    for (auto const &aux_contract : aux_contracts) {
      const auto *aux_auth_data =
          co_await get_account(current_block_id_, aux_contract);
      if (!aux_auth_data) {
        co_return false;
      }
      aux_auth_map[aux_contract] = aux_auth_data;
    }

    auto r = verify_auth(tx, *auth_data, aux_auth_map);
    co_return r.has_value();
  }

  static SignedTransaction sign_claims(
      const Transaction &tx,
      const std::unordered_map<uint32_t, Ed25519SecretKey>
          &account_private_keys,
      const std::unordered_map<ContractAddress,
                               std::unordered_map<uint32_t, Ed25519SecretKey>>
          &auxiliary_private_keys)
  {
    const auto msg = sha256(tx);
    SignedTransaction signed_tx{.tx = tx, .signatures = {}};
    auto make_sig = [&](Ed25519SecretKey private_key, uint32_t idx) {
      Signature sig(CryptographicScheme::Ed25519);
      sig.ed25519() = ed25519_sign(private_key, msg);
      return DecoratedSignature{.public_key_id = idx, .signature = sig};
    };

    for (auto const &auth_claim : tx.auth_claims) {
      DecoratedSignatureList sigs;
      switch (auth_claim.claim_type()) {
      case AuthClaimType::kInvokedAddressSignature:
        sigs.push_back(make_sig(account_private_keys.at(
                                    auth_claim.invoked_address_signature_id()),
                                auth_claim.invoked_address_signature_id()));
        break;
      case AuthClaimType::kInvokedAddressSignedWeight:
        throw std::runtime_error("kInvokedAddressSignedWeight not implemented");
        break;
      case AuthClaimType::kOtherAddressSignature:
        sigs.push_back(
            make_sig(auxiliary_private_keys
                         .at(auth_claim.other_address_signature_id().address)
                         .at(auth_claim.other_address_signature_id().key_id),
                     auth_claim.other_address_signature_id().key_id));
        break;
      case AuthClaimType::kOtherAddressSignedWeight:
        throw std::runtime_error("kOtherAddressSignedWeight not implemented");
        break;
      }
      signed_tx.signatures.push_back(sigs);
    }
    return signed_tx;
  }

private:
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
      co_return it->second.get();
    }

    DbAccountAuthDataBatchResult res =
        co_await db_sched_client_.dbsched_get_account_auth_data(block_id,
                                                                {account});
    if (res.result() != DbSchedResult::DbSchedResultOK) {
      co_return nullptr;
    }
    CHECK_EQ(res.account_auth_data_batch().size(), 1UL);
    accounts_[account] =
        std::make_unique<AccountAuthData>(res.account_auth_data_batch()[0]);
    co_return accounts_[account].get();
  }

  BlockID current_block_id_;
  db_sched_client_t &db_sched_client_;
  std::unordered_map<ContractAddress, std::unique_ptr<AccountAuthData>>
      accounts_;
};

} // namespace hogd
