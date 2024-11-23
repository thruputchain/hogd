#pragma once

#include <cstdint>
#include <memory>
#include <optional>

#include "contract.hh"
#include "hogd/hogexec/runtime/runtime_test_framework.h"
#include "hogd/hogexec/runtime/wasmi_runtime.h"
#include "types.hh"

namespace hogd ::test ::erc20 {

SignedTransaction make_deploy_wallet_transaction(
    SHA256Hash const &nonce, SHA256Hash const &h_wallet,
    std::shared_ptr<const Contract> const &c_wallet, Ed25519PublicKey const &pk,
    ContractAddress const &erc20_addr);
SignedTransaction
make_deploy_erc20_noinit(SHA256Hash const &nonce, SHA256Hash const &h_erc20,
                         std::shared_ptr<const Contract> const &c_erc20);

SignedTransaction make_initialize_erc20(ContractAddress const &owner,
                                        ContractAddress const &erc20_address);

SignedTransaction make_mint_transaction(ContractAddress const &manager_wallet,
                                        ContractAddress const &to,
                                        int64_t amount);

SignedTransaction make_check_balance_transaction(ContractAddress const &wallet,
                                                 int64_t value);

SignedTransaction make_transfer_transaction(ContractAddress const &sender,
                                            ContractAddress const &to,
                                            int64_t amount);

SignedTransaction make_unauthorized_transferFrom_transaction(
    ContractAddress const &erc20_address, ContractAddress const &owner,
    ContractAddress const &to, int64_t amount);

std::optional<ContractAddress>
deploy_erc20_noinit(RuntimeTestFramework &runtime_framework);
std::optional<ContractAddress>
deploy_and_initialize_wallet(RuntimeTestFramework &runtime_framework,
                             ContractAddress const &erc20_address,
                             uint64_t nonce);

bool initialize_erc20(RuntimeTestFramework &runtime_framework,
                      ContractAddress const &manager_address,
                      ContractAddress const &erc20_address);

bool mint_to(RuntimeTestFramework &runtime_framework,
             ContractAddress const &manager_address,
             ContractAddress const &wallet, int64_t amount);

bool send_payment(RuntimeTestFramework &runtime_framework,
                  ContractAddress const &from, ContractAddress const &to,
                  int64_t amount,
                  std::shared_ptr<WasmiContext> wasmi_context = nullptr);

} // namespace hogd::test::erc20
