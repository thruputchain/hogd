#include "hogd/hogexec/precompiles/precompile_native_token.h"

#include <cassert>
#include <limits>
#include <memory>
#include <type_traits>

#include "ffi_enums.hh"
#include "hogd/base/compiler.h"
#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h"
#include "hogd/crypto/crypto.h"
#include "hogd/hogexec/precompiles/native_contract_builder.h"
#include "hogd/hogexec/runtime/contract_result.h"
#include "hogd/hogexec/runtime/native_runtime.h"
#include "hogd/hogexec/runtime/serialize_calldata.h"
#include "hogd/hogexec/runtime/status.h"
#include "hogd/hogexec/runtime/syscall_handler.h"
#include "test_contracts/itc_erc20.h"
#include "test_contracts/itc_erc20.hh"
#include "types.hh"

namespace hogd {

namespace {

StorageKeyContractData
allowance_key(ContractAddress owner, ContractAddress spender)
{
  auto data = Serializer<SerializationFormat::kTxs>::serialize(owner, spender);
  assert(data.has_value());
  return sha256(*data); // NOLINT(bugprone-unchecked-optional-access)
}

SystemStatus<bool>
adjust_allowance(SystemCallHandler &handler, ContractAddress owner,
                 ContractAddress spender, int128_t amount)
{
  auto allow_key = allowance_key(owner, spender);
  auto r = handler.get_nnint(allow_key, true);
  if (!r.has_value()) {
    return r.error();
  }
  if (!r.value().has_value()) {
    return false;
  }
  auto [set, add] = r.value().value();
  if (integer_add_overflow(add, amount)) {
    return false;
  }
  int128_t new_add = add + amount;
  assert(set.has_value());
  auto r2 = handler.set_nnint(allow_key, set, new_add);
  if (!r2.has_value()) {
    return r2.error();
  }
  return r2.value().has_value();
}

// For now, the typedefs exported by a contract aren't added to the generated
// spec, unfortunately
using amount_t = uint128_xdr;
using difference_t = int128_xdr;

} // namespace

std::unique_ptr<NativeContract>
make_native_token_contract()
{
  NativeContractBuilder builder;

  // initialize
  builder.add<itc_erc20::ERC20, "initialize">(
      kAlwaysErrorMethod<itc_erc20::ERC20, "initialize", 1>);

  // transfer
  builder.add<itc_erc20::ERC20, "transfer">(
      [](SystemCallHandler &handler, NativeRuntime &runtime,
         ContractAddress receiver,
         amount_t amount) -> InvokeStatus<Result<void, itc_erc20::ERC20Error>> {
        if (!runtime.consume_gas(1)) {
          return InvokeError{InvokeErrorCode::kInvokeOutOfGas};
        }

        // For now, amount_t is a uint, so we do not need to check for negative
        // amounts here. adding the assertion in case that changes
        static_assert(std::is_same_v<amount_t, uint128_xdr>);

        auto owner = handler.get_msg_sender();
        if (!owner.has_value()) {
          return InvokeError{InvokeErrorCode::kInvokeDeterministic};
        }

        // can't negate bigger values
        if (from_xdr(amount) > std::numeric_limits<int128_t>::max()) {
          return InvokeError{InvokeErrorCode::kInvokeDeterministic};
        }
        int128_t amount_int = from_xdr(amount);

        // subtract amount from owner's balance
        auto r1 = handler.modify_native_token_balance(*owner, -amount_int);
        if (!r1.has_value()) {
          return r1.error().to_invoke_error();
        }
        if (!r1.value().has_value()) {
          return InvokeError{InvokeErrorCode::kInvokeDeterministic};
        }

        // add amount to receiver's balance
        auto r2 = handler.modify_native_token_balance(receiver, amount_int);
        if (!r2.has_value()) {
          return r2.error().to_invoke_error();
        }
        if (!r2.value().has_value()) {
          return InvokeError{InvokeErrorCode::kInvokeDeterministic};
        }

        return {Result<void, itc_erc20::ERC20Error>::make_ok()};
      });

  // transfer_from
  builder.add<itc_erc20::ERC20, "transfer_from">(
      [](SystemCallHandler &handler, NativeRuntime &runtime,
         ContractAddress owner, ContractAddress receiver,
         amount_t amount) -> InvokeStatus<Result<void, itc_erc20::ERC20Error>> {
        if (!runtime.consume_gas(1)) {
          return InvokeError{InvokeErrorCode::kInvokeOutOfGas};
        }

        // For now, amount_t is a uint, so we do not need to check for negative
        // amounts here. adding the assertion in case that changes
        static_assert(std::is_same_v<amount_t, uint128_xdr>);

        // get spender
        auto spender = handler.get_msg_sender();
        if (!spender.has_value()) {
          return InvokeError{InvokeErrorCode::kInvokeDeterministic};
        }

        // can't negate bigger values
        if (from_xdr(amount) > std::numeric_limits<int128_t>::max()) {
          return InvokeError{InvokeErrorCode::kInvokeDeterministic};
        }
        int128_t amount_int = from_xdr(amount);

        // subtract amount from owner's balance
        auto r1 = handler.modify_native_token_balance(owner, -amount_int);
        if (!r1.has_value()) {
          return r1.error().to_invoke_error();
        }
        if (!r1.value()) {
          return InvokeError{InvokeErrorCode::kInvokeDeterministic};
        }

        // add amount to receiver's balance
        auto r2 = handler.modify_native_token_balance(receiver, amount_int);
        if (!r2.has_value()) {
          return r2.error().to_invoke_error();
        }
        if (!r2.value().has_value()) {
          return InvokeError{InvokeErrorCode::kInvokeDeterministic};
        }

        // subtract amount from spender's allowance
        auto allow_res =
            adjust_allowance(handler, owner, *spender, -amount_int);
        if (!allow_res.has_value()) {
          return allow_res.error().to_invoke_error();
        }
        if (!allow_res.value()) {
          return InvokeError{InvokeErrorCode::kInvokeDeterministic};
        }

        return {Result<void, itc_erc20::ERC20Error>::make_ok()};
      });

  // adjust_allowance
  builder.add<itc_erc20::ERC20, "adjust_allowance">(
      [](SystemCallHandler &handler, NativeRuntime &runtime,
         ContractAddress spender, difference_t amount)
          -> InvokeStatus<Result<void, itc_erc20::ERC20Error>> {
        if (!runtime.consume_gas(1)) {
          return InvokeError{InvokeErrorCode::kInvokeOutOfGas};
        }

        auto owner = handler.get_msg_sender();
        if (!owner.has_value()) {
          return InvokeError{InvokeErrorCode::kInvokeDeterministic};
        }

        auto r = adjust_allowance(handler, *owner, spender, from_xdr(amount));
        if (!r.has_value()) {
          return r.error().to_invoke_error();
        }
        if (!r.value()) {
          return InvokeError{InvokeErrorCode::kInvokeDeterministic};
        }
        return {Result<void, itc_erc20::ERC20Error>::make_ok()};
      });

  // get_balance_of
  builder.add<itc_erc20::ERC20, "get_balance_of">(
      [](SystemCallHandler &handler, NativeRuntime &runtime,
         ContractAddress contract_address)
          -> InvokeStatus<Result<amount_t, itc_erc20::ERC20Error>> {
        if (!runtime.consume_gas(1)) {
          return InvokeError{InvokeErrorCode::kInvokeOutOfGas};
        }
        auto balance = handler.get_native_token_balance(contract_address);
        if (!balance.has_value()) {
          return balance.error().to_invoke_error();
        }
        if (!balance.value().has_value()) {
          return InvokeError{InvokeErrorCode::kInvokeDeterministic};
        }
        assert(balance.value().value() >= 0);
        uint128_t ubalance = balance.value().value();
        return {
            Result<amount_t, itc_erc20::ERC20Error>::make_ok(to_xdr(ubalance))};
      });

  // get_balance
  builder.add<itc_erc20::ERC20, "get_balance">(
      [](SystemCallHandler &handler, NativeRuntime &runtime)
          -> InvokeStatus<Result<amount_t, itc_erc20::ERC20Error>> {
        if (!runtime.consume_gas(1)) {
          return InvokeError{InvokeErrorCode::kInvokeOutOfGas};
        }

        auto caller_address = handler.get_msg_sender();
        if (!caller_address.has_value()) {
          return InvokeError{InvokeErrorCode::kInvokeDeterministic};
        }

        auto balance = handler.get_native_token_balance(*caller_address);
        if (!balance.has_value()) {
          return balance.error().to_invoke_error();
        }
        if (!balance.value().has_value()) {
          return InvokeError{InvokeErrorCode::kInvokeDeterministic};
        }
        assert(balance.value().value() >= 0);
        uint128_t ubalance = balance.value().value();
        return {
            Result<amount_t, itc_erc20::ERC20Error>::make_ok(to_xdr(ubalance))};
      });

  // mint
  builder.add<itc_erc20::ERC20, "mint">(
      kAlwaysErrorMethod<itc_erc20::ERC20, "mint", 1>);

  // burn
  builder.add<itc_erc20::ERC20, "burn">(
      kAlwaysErrorMethod<itc_erc20::ERC20, "burn", 1>);

  // total_supply
  builder.add<itc_erc20::ERC20, "total_supply">(
      [](NativeRuntime &runtime)
          -> InvokeStatus<Result<amount_t, itc_erc20::ERC20Error>> {
        // TODO(geoff): gas costs
        if (!runtime.consume_gas(1)) {
          return InvokeError{InvokeErrorCode::kInvokeOutOfGas};
        }
        return {Result<amount_t, itc_erc20::ERC20Error>::make_ok(
            to_xdr(uint128_t{kTotalHogSupply} * uint128_t{kHogDivisibility}))};
      });

  return builder.build();
}

} // namespace hogd
