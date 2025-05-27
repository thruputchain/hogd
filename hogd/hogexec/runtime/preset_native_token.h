#pragma once

#include <array>
#include <memory>
#include <string_view>
#include <tuple>

#include "hogd/base/types.h"
#include "hogd/hogexec/runtime/native_runtime.h"
#include "types.hh"

namespace hogd {

std::unique_ptr<NativeContract> make_native_token_contract();

/*
pub trait ERC20 {
    fn initialize(
        maybe_self: Option<&mut Self>,
        contract_owner: &ContractAddress,
    ) -> Result<()>;

    /// Transfer from caller to `receiver` address
    fn transfer(
        &mut self,
        receiver: &ContractAddress,
        amount: Amount,
    ) -> Result<()>;

    /// Transfer from `owner` to `receiver` address, using allowance
    fn transfer_from(
        &mut self,
        owner: &ContractAddress,
        receiver: &ContractAddress,
        amount: Amount,
    ) -> Result<()>;

    /// Adjust allowance for `spender` by `amount` (positive or negative)
    fn adjust_allowance(
        &mut self,
        spender: &ContractAddress,
        amount: Amount,
    ) -> Result<()>;

    /// Get balance for given address
    fn get_balance_of(&self, addr: &ContractAddress) -> Result<Amount>;

    /// Get balance for caller
    fn get_balance(&self) -> Result<Amount>;

    /// Create tokens into `receiver` address
    fn mint(
        &mut self,
        receiver: &ContractAddress,
        amount: Amount,
    ) -> Result<()>;

    /// Destroy tokens from `owner` address
    fn burn(&mut self, owner: &ContractAddress, amount: Amount) -> Result<()>;

    /// Get total supply of outstanding tokens
    fn total_supply(&self) -> Result<Amount>;
}
*/

// Note: the native token doesn't actually allow
// initialize()/mint()/burn().  We have those as placeholders here
// so that method IDs are calculated correctly, and return appropriate errors
// to match the expected behavior of other tokens.
struct NativeTokenTrait {
  constexpr static std::string_view kTraitName = "NativeToken";
  constexpr static std::string_view kVersion = "0.1.0";

  constexpr static auto kExportNames =
      std::array<std::string_view, 9>{"initialize",     "transfer",
                                      "transfer_from",  "adjust_allowance",
                                      "get_balance_of", "get_balance",
                                      "mint",           "burn",
                                      "total_supply"};

  using export_types =
      std::tuple<void (*)(ContractAddress),           // initialize
                 void (*)(ContractAddress, int128_t), // transfer
                 void (*)(ContractAddress, ContractAddress,
                          int128_t),                  // transfer_from
                 void (*)(ContractAddress, int128_t), // adjust_allowance
                 int128_t (*)(ContractAddress),       // get_balance_of
                 int128_t (*)(),                      // get_balance
                 void (*)(ContractAddress, int128_t), // mint
                 void (*)(ContractAddress, int128_t), // burn
                 int128_t (*)(ContractAddress)        // total_supply
                 >;
};

} // namespace hogd
