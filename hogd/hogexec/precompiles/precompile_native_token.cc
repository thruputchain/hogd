#include "hogd/hogexec/precompiles/precompile_native_token.h"

#include <cassert>
#include <memory>

#include "ffi_enums.hh"
#include "hogd/base/compiler.h"
#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h"
#include "hogd/crypto/crypto.h"
#include "hogd/hogexec/precompiles/native_contract_builder.h"
#include "hogd/hogexec/runtime/native_runtime.h"
#include "hogd/hogexec/runtime/serialize_calldata.h"
#include "hogd/hogexec/runtime/status.h"
#include "hogd/hogexec/runtime/syscall_handler.h"
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

} // namespace

std::unique_ptr<NativeContract>
make_native_token_contract()
{
  NativeContractBuilder builder;

  // initialize
  builder.add<NativeTokenTrait, "initialize">(
      kAlwaysErrorMethod<NativeTokenTrait, "initialize", 1>);

  // transfer
  builder.add<NativeTokenTrait, "transfer">(
      [](SystemCallHandler &handler, NativeRuntime &runtime,
         ContractAddress receiver,
         NativeTokenTrait::amount_t amount) -> InvokeStatus<void> {
        if (!runtime.consume_gas(1)) {
          return InvokeError{InvokeErrorCode::kInvokeOutOfGas};
        }

        if (from_xdr(amount) < 0) {
          return InvokeError{InvokeErrorCode::kInvokeDeterministic};
        }

        auto owner = handler.get_msg_sender();
        if (!owner.has_value()) {
          return InvokeError{InvokeErrorCode::kInvokeDeterministic};
        }

        // subtract amount from owner's balance
        auto r1 =
            handler.modify_native_token_balance(*owner, -from_xdr(amount));
        if (!r1.has_value()) {
          return r1.error().to_invoke_error();
        }
        if (!r1.value().has_value()) {
          return InvokeError{InvokeErrorCode::kInvokeDeterministic};
        }

        // add amount to receiver's balance
        auto r2 =
            handler.modify_native_token_balance(receiver, from_xdr(amount));
        if (!r2.has_value()) {
          return r2.error().to_invoke_error();
        }
        if (!r2.value().has_value()) {
          return InvokeError{InvokeErrorCode::kInvokeDeterministic};
        }

        return {};
      });

  // transfer_from
  builder.add<NativeTokenTrait, "transfer_from">(
      [](SystemCallHandler &handler, NativeRuntime &runtime,
         ContractAddress owner, ContractAddress receiver,
         NativeTokenTrait::amount_t amount) -> InvokeStatus<void> {
        if (!runtime.consume_gas(1)) {
          return InvokeError{InvokeErrorCode::kInvokeOutOfGas};
        }

        if (from_xdr(amount) < 0) {
          return InvokeError{InvokeErrorCode::kInvokeDeterministic};
        }

        // get spender
        auto spender = handler.get_msg_sender();
        if (!spender.has_value()) {
          return InvokeError{InvokeErrorCode::kInvokeDeterministic};
        }

        // subtract amount from owner's balance
        auto r1 = handler.modify_native_token_balance(owner, -from_xdr(amount));
        if (!r1.has_value()) {
          return r1.error().to_invoke_error();
        }
        if (!r1.value()) {
          return InvokeError{InvokeErrorCode::kInvokeDeterministic};
        }

        // add amount to receiver's balance
        auto r2 =
            handler.modify_native_token_balance(receiver, from_xdr(amount));
        if (!r2.has_value()) {
          return r2.error().to_invoke_error();
        }
        if (!r2.value().has_value()) {
          return InvokeError{InvokeErrorCode::kInvokeDeterministic};
        }

        // subtract amount from spender's allowance
        auto allow_res =
            adjust_allowance(handler, owner, *spender, -from_xdr(amount));
        if (!allow_res.has_value()) {
          return allow_res.error().to_invoke_error();
        }
        if (!allow_res.value()) {
          return InvokeError{InvokeErrorCode::kInvokeDeterministic};
        }

        return {};
      });

  // adjust_allowance
  builder.add<NativeTokenTrait, "adjust_allowance">(
      [](SystemCallHandler &handler, NativeRuntime &runtime,
         ContractAddress spender,
         NativeTokenTrait::amount_t amount) -> InvokeStatus<void> {
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
        return {};
      });

  // get_balance_of
  builder.add<NativeTokenTrait, "get_balance_of">(
      [](SystemCallHandler &handler, NativeRuntime &runtime,
         ContractAddress contract_address)
          -> InvokeStatus<NativeTokenTrait::amount_t> {
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
        return to_xdr(balance.value().value());
      });

  // get_balance
  builder.add<NativeTokenTrait, "get_balance">(
      [](SystemCallHandler &handler,
         NativeRuntime &runtime) -> InvokeStatus<NativeTokenTrait::amount_t> {
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
        return to_xdr(balance.value().value());
      });

  // mint
  builder.add<NativeTokenTrait, "mint">(
      kAlwaysErrorMethod<NativeTokenTrait, "mint", 1>);

  // burn
  builder.add<NativeTokenTrait, "burn">(
      kAlwaysErrorMethod<NativeTokenTrait, "burn", 1>);

  // total_supply
  builder.add<NativeTokenTrait, "total_supply">(
      [](NativeRuntime &runtime) -> InvokeStatus<NativeTokenTrait::amount_t> {
        // TODO(geoff): gas costs
        if (!runtime.consume_gas(1)) {
          return InvokeError{InvokeErrorCode::kInvokeOutOfGas};
        }
        return to_xdr(int128_t{kTotalHogSupply} * int128_t{kHogDivisibility});
      });

  return builder.build();
}

} // namespace hogd
