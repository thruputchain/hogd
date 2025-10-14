#pragma once

#include <array>
#include <cstring>
#include <limits>
#include <memory>

#include "account.hh"
#include "built_contracts/fibonacci.h"
#include "exec_sched.hh"
#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "hogd/crypto/crypto.h"
#include "hogd/hogexec/runtime/contract_trait.h"
#include "transaction.hh"
#include "types.hh"
#include "x23/vec.h"
#include "x23/x23.h"

namespace hogd {

using fib_client_t = ContractTraitClient<fibonacci::Fibonacci>;

// TODO(jhumphri): why is this not just returning a Vec<Transaction>?
inline SignedTransactionBatch
generate_unsigned_txs(fib_client_t &fib_client, size_t num_txs)
{
  constexpr char kProgramName[] = "slow_fibonacci";
  constexpr uint32_t kFibonacciN = 10;

  SignedTransactionBatch txs;
  txs.reserve(num_txs);
  for (uint64_t i = 0; i < num_txs; i++) {
    SignedTransaction tx{
        .tx = Transaction{
            .invocation = fib_client.make_invocation<kProgramName>(
                // TODO(jhumphri): How to get this number automatically?
                /*gas_amount=*/5'000'000'000, kFibonacciN),
            .uniquifier = {.block_expiry_height =
                               std::numeric_limits<BlockHeight>::max(),
                           .nonce = i},
            .contracts_to_deploy = {}}};
    txs.push_back(tx);
  }
  return txs;
}

inline AccountAuthData
get_default_account()
{
  static std::array<PublicKeyConfigOpt, MAX_SIGNERS> public_keys;
  if (public_keys.empty()) {
    // Initialize [public_keys].
    for (size_t i = 0; i < MAX_SIGNERS; i++) {
      public_keys[i] = nullptr;
    }
  }
  CHECK_EQ(public_keys.size(), static_cast<size_t>(MAX_SIGNERS));
  constexpr NonceConfig kNonceConfig{.accepted_keys = 0, .required_weight = 0};

  return AccountAuthData{.public_keys = public_keys,
                         .gas_permissions = {},
                         .nonce_config = kNonceConfig};
}

inline Ed25519SecretKey
create_key_pair(AccountAuthData &auth_data, size_t idx, Weight weight)
{
  const auto [public_key, private_key] = ed25519_keygen();
  x23::Ptr<PublicKeyConfig> &pk_config = auth_data.public_keys[idx];
  pk_config = x23::Ptr<PublicKeyConfig>(new PublicKeyConfig());
  pk_config->key = PublicKey(CryptographicScheme::Ed25519);
  pk_config->key.ed25519() = public_key;
  pk_config->max_weight = weight;
  return private_key;
}

inline void
set_nonce_key(AccountAuthData &auth_data, size_t idx)
{
  CHECK_LT(idx, sizeof(auth_data.nonce_config.accepted_keys));
  auth_data.nonce_config.accepted_keys |= (1 << idx);
}

inline void
add_gas_config(AccountAuthData &auth_data, GasAmount gas,
               const x23::Vec<size_t> &keys, Weight required_weight)
{
  CHECK_LT(auth_data.gas_permissions.size(),
           static_cast<size_t>(MAX_GAS_CONFIGS));
  GasPermissionConfig config{.limit = gas,
                             .accepted_keys = 0,
                             .required_weight = required_weight};
  for (const size_t idx : keys) {
    config.accepted_keys |= (1 << idx);
  }
  auth_data.gas_permissions.push_back(config);
}

} // namespace hogd
