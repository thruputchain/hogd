#include "hogd/hogexec/runtime/preset_native_token.h"

#include <cassert>
#include <memory>

#include "ffi_enums.hh"
#include "hogd/base/compiler.h"
#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h"
#include "hogd/crypto/crypto.h"
#include "hogd/hogexec/runtime/contract_trait.h"
#include "hogd/hogexec/runtime/native_runtime.h"
#include "hogd/hogexec/runtime/status.h"
#include "hogd/hogexec/runtime/syscall_handler.h"
#include "types.hh"
#include "x23/serialize.h"

namespace hogd {

namespace {

// this doesn't exactly match a token behavior -- we're not checking calldata
// but that doesn't strictly matter.
auto always_error = [](NativeRuntime &runtime) -> InvokeStatus<void> {
  if (!runtime.consume_gas(1)) {
    return InvokeError{InvokeErrorCode::kInvokeOutOfGas};
  }
  return InvokeError{InvokeErrorCode::kInvokeDeterministic};
};

StorageKeyContractData
allowance_key(ContractAddress owner, ContractAddress spender)
{
  auto data = x23::serialize(owner, spender);
  return sha256(data);
}

SystemStatus<bool>
adjust_allowance(SystemCallHandler &handler, ContractAddress owner,
                 ContractAddress spender, int128_t amount)
{
  auto allow_key = allowance_key(owner, spender);
  auto r = handler.get_nnint(allow_key);
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

  using trait_t = ContractTraitMeta<NativeTokenTrait>;

  // initialize
  builder.add(sdk_method_id(trait_t::entrypoint<"initialize">()), always_error);

  // transfer
  builder.add(sdk_method_id(trait_t::entrypoint<"transfer">()),
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
                    handler.modify_native_token_balance(*owner,
                                                        -from_xdr(amount));
                if (!r1.has_value()) {
                  return r1.error().to_invoke_error();
                }
                if (!r1.value().has_value()) {
                  return InvokeError{InvokeErrorCode::kInvokeDeterministic};
                }

                // add amount to receiver's balance
                auto r2 = handler.modify_native_token_balance(receiver,
                                                              from_xdr(amount));
                if (!r2.has_value()) {
                  return r2.error().to_invoke_error();
                }
                if (!r2.value().has_value()) {
                  return InvokeError{InvokeErrorCode::kInvokeDeterministic};
                }

                return {};
              });

  // transfer_from
  builder.add(sdk_method_id(trait_t::entrypoint<"transfer_from">()),
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
                auto r1 =
                    handler.modify_native_token_balance(owner,
                                                        -from_xdr(amount));
                if (!r1.has_value()) {
                  return r1.error().to_invoke_error();
                }
                if (!r1.value()) {
                  return InvokeError{InvokeErrorCode::kInvokeDeterministic};
                }

                // add amount to receiver's balance
                auto r2 = handler.modify_native_token_balance(receiver,
                                                              from_xdr(amount));
                if (!r2.has_value()) {
                  return r2.error().to_invoke_error();
                }
                if (!r2.value().has_value()) {
                  return InvokeError{InvokeErrorCode::kInvokeDeterministic};
                }

                // subtract amount from spender's allowance
                auto allow_res = adjust_allowance(handler, owner, *spender,
                                                  -from_xdr(amount));
                if (!allow_res.has_value()) {
                  return allow_res.error().to_invoke_error();
                }
                if (!allow_res.value()) {
                  return InvokeError{InvokeErrorCode::kInvokeDeterministic};
                }

                return {};
              });

  // adjust_allowance
  builder.add(sdk_method_id(trait_t::entrypoint<"adjust_allowance">()),
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

                auto r = adjust_allowance(handler, *owner, spender,
                                          from_xdr(amount));
                if (!r.has_value()) {
                  return r.error().to_invoke_error();
                }
                if (!r.value()) {
                  return InvokeError{InvokeErrorCode::kInvokeDeterministic};
                }
                return {};
              });

  // get_balance_of
  builder.add(sdk_method_id(trait_t::entrypoint<"get_balance_of">()),
              [](SystemCallHandler &handler, NativeRuntime &runtime,
                 ContractAddress contract_address)
                  -> InvokeStatus<NativeTokenTrait::amount_t> {
                if (!runtime.consume_gas(1)) {
                  return InvokeError{InvokeErrorCode::kInvokeOutOfGas};
                }
                auto balance =
                    handler.get_native_token_balance(contract_address);
                if (!balance.has_value()) {
                  return balance.error().to_invoke_error();
                }
                if (!balance.value().has_value()) {
                  return InvokeError{InvokeErrorCode::kInvokeDeterministic};
                }
                return to_xdr(balance.value().value());
              });

  // get_balance
  builder.add(sdk_method_id(trait_t::entrypoint<"get_balance">()),
              [](SystemCallHandler &handler, NativeRuntime &runtime)
                  -> InvokeStatus<NativeTokenTrait::amount_t> {
                if (!runtime.consume_gas(1)) {
                  return InvokeError{InvokeErrorCode::kInvokeOutOfGas};
                }

                auto caller_address = handler.get_msg_sender();
                if (!caller_address.has_value()) {
                  return InvokeError{InvokeErrorCode::kInvokeDeterministic};
                }

                auto balance =
                    handler.get_native_token_balance(*caller_address);
                if (!balance.has_value()) {
                  return balance.error().to_invoke_error();
                }
                if (!balance.value().has_value()) {
                  return InvokeError{InvokeErrorCode::kInvokeDeterministic};
                }
                return to_xdr(balance.value().value());
              });

  // mint
  builder.add(sdk_method_id(trait_t::entrypoint<"mint">()), always_error);

  // burn
  builder.add(sdk_method_id(trait_t::entrypoint<"burn">()), always_error);

  // total_supply
  builder.add(
      sdk_method_id(trait_t::entrypoint<"total_supply">()),
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
