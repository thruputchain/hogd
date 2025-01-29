#include "hogd/loadgen/payment_experiment.h"

#include "hogd/crypto/crypto.h"

namespace hogd {

PaymentExperiment::PaymentExperiment(uint32_t num_accounts)
  : num_accounts_(num_accounts)
{
  generate_accounts();
}

void
PaymentExperiment::generate_accounts()
{
  // TODO(geoff): this is probably very slow.  Can be parallelized.
  for (uint32_t i = 0; i < num_accounts_; ++i) {
    auto [public_key, secret_key] = deterministic_keypair(i);
    ContractAddress wallet_address = sha256(public_key);
    accounts_.emplace(i, Account{.wallet_address_ = wallet_address,
                                 .public_key_ = public_key,
                                 .secret_key_ = secret_key});
  }
}

std::vector<ProposedTransaction>
PaymentExperiment::make_deploy_wallet_transactions()
{
  // TODO(geoff): implement
  return {};
}

std::vector<std::vector<ProposedTransaction>>
PaymentExperiment::produce_initialization_transactions()
{
  // TODO(geoff): implement
  return {};
}

} // namespace hogd
