#pragma once

#include <array>
#include <cstring>
#include <limits>
#include <memory>

#include "address_auth.hh"
#include "built_contracts/fibonacci.h"
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
            .contracts_to_deploy = {},
        }};
    txs.push_back(tx);
  }
  return txs;
}

inline Ed25519SecretKey
create_key_pair(AddressAuthData &auth_data, size_t idx, Weight weight)
{
  const auto [public_key, private_key] = ed25519_keygen();
  x23::Ptr<PublicKeyConfig> &pk_config = auth_data.public_keys[idx];
  pk_config = x23::Ptr<PublicKeyConfig>(new PublicKeyConfig());
  pk_config->key = PublicKey(CryptographicScheme::Ed25519);
  pk_config->key.ed25519() = public_key;
  pk_config->max_weight = weight;
  return private_key;
}

inline AddressAuthData
get_default_account()
{
  std::array<PublicKeyConfigOpt, kMaxSigners> public_keys;
  // Initialize [public_keys].
  for (size_t i = 0; i < kMaxSigners; i++) {
    public_keys[i] = nullptr;
  }

  return AddressAuthData{.public_keys = public_keys, .required_weight = 0};
}

inline AuthClaim
make_sign_claim(uint32_t idx)
{
  AuthClaim claim(AuthClaimType::kSourceAddressAuthData);
  claim.source_address_claim().allowed_keys = 1 << idx;
  claim.source_address_claim().required_weight = 1;
  return claim;
}

// NOLINTBEGIN(bugprone-unchecked-optional-access)
inline DecoratedSignature
make_signature(const SecretKey &sk, const Transaction &tx,
               unsigned int public_key_id)
{
  DecoratedSignature signature;
  signature.public_key_id = public_key_id;
  signature.signature = Signature(CryptographicScheme::Ed25519);
  signature.signature.ed25519() =
      ed25519_sign(sk.ed25519(), x23::serialize(tx)).value();
  return signature;
}
// NOLINTEND(bugprone-unchecked-optional-access)

} // namespace hogd
