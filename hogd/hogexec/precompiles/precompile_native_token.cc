#include "hogd/hogexec/precompiles/precompile_native_token.h"

#include <algorithm>
#include <cassert>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <tuple>
#include <utility>

#include "built_contracts/itc_erc20.h"
#include "built_contracts/itc_erc20.hh"
#include "event.hh"
#include "ffi_enums.hh"
#include "hogd/base/compiler.h"
#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h"
#include "hogd/crypto/crypto.h"
#include "hogd/event/utilities.h"
#include "hogd/hogexec/common/status.h"
#include "hogd/hogexec/components/contract_result.h"
#include "hogd/hogexec/precompiles/native_contract_builder.h"
#include "hogd/hogexec/runtime/native_runtime.h"
#include "hogd/hogexec/runtime/native_syscall_context.h"
#include "hogd/hogexec/runtime/serialize_calldata.h"
#include "hogd/hogexec/runtime/syscall_handler.h"
#include "standard_asset_events.hh"
#include "storage_item.hh"
#include "syscall.hh"
#include "syscall_error.hh"
#include "types.hh"
#include "x23/format.h"
#include "x23/vec.h"

namespace hogd {

using ERC20Exception =
    ContractException<itc_erc20::ERC20Error, 0xb19947acb2fa14ffULL>;

namespace {

// Emit an event through the kLogEvent syscall.
// The event_buf must contain topics concatenated with data (as produced by
// make_event_from_xdr_struct). num_topics topics of sizeof(Topic) bytes each
// are split off the front; the remainder is data.
template<uint32_t NumTopics>
InvokeStatus<void>
emit_event(NativeSyscallContext &ctx, x23::Buf const &event_buf)
{
  static_assert(NumTopics <= 3);
  auto buf = std::as_bytes(std::span(event_buf));

  const Topic *t0 = nullptr;
  const Topic *t1 = nullptr;
  const Topic *t2 = nullptr;

  Topic topics[3];
  for (uint32_t i = 0; i < NumTopics; ++i) {
    std::memcpy(&topics[i], buf.data() + (i * sizeof(Topic)), sizeof(Topic));
  }
  if constexpr (NumTopics >= 1)
    t0 = &topics[0];
  if constexpr (NumTopics >= 2)
    t1 = &topics[1];
  if constexpr (NumTopics >= 3)
    t2 = &topics[2];

  auto data = buf.subspan(NumTopics * sizeof(Topic));
  auto r = ctx.syscall<SyscallNo::kLogEvent>(t0, t1, t2, data);
  if (!r.has_value()) {
    return NativeContractBuilder::host_fn_error_conversion(r.error());
  }
  if (*r != SyscallErrorCode::kSyscallErrorCodeNone) {
    return InvokeError{InvokeErrorCode::Deterministic};
  }
  return {};
}

// Get the msg_sender via the kGetMsgSender syscall.
InvokeStatus<ContractAddress>
get_msg_sender(NativeSyscallContext &ctx)
{
  ContractAddress sender{};
  auto r = ctx.syscall<SyscallNo::kGetMsgSender>(sender);
  if (!r.has_value()) {
    return NativeContractBuilder::host_fn_error_conversion(r.error());
  }
  if (*r != SyscallErrorCode::kSyscallErrorCodeNone) {
    return InvokeError{InvokeErrorCode::Deterministic};
  }
  return sender;
}

StorageKeyContractData
allowance_key(ContractAddress owner, ContractAddress spender)
{
  auto data = Serializer<SerializationFormat::kTxs>::serialize(owner, spender);
  assert(data.has_value());
  return sha256(*data); // NOLINT(bugprone-unchecked-optional-access)
}

InvokeStatus<bool>
adjust_allowance(NativeSyscallContext &ctx, ContractAddress owner,
                 ContractAddress spender, int128_t amount)
{
  auto allow_key = allowance_key(owner, spender);

  int128_t output_set{};
  bool output_set_opt{};
  int128_t output_add{};
  auto r =
      ctx.syscall<SyscallNo::kNonnegativeIntGet>(allow_key, false, output_set,
                                                 output_set_opt, output_add);
  if (!r.has_value()) {
    return NativeContractBuilder::host_fn_error_conversion(r.error());
  }
  if (*r != SyscallErrorCode::kSyscallErrorCodeNone) {
    return false;
  }

  if (integer_add_overflow(output_add, amount)) {
    return false;
  }
  int128_t new_add = output_add + amount;

  const int128_t *set_ptr = output_set_opt ? &output_set : nullptr;
  auto r2 = ctx.syscall<SyscallNo::kNonnegativeIntSetAdd>(
      allow_key, set_ptr, new_add, std::to_underlying(StorageDeltaFlags::None));
  if (!r2.has_value()) {
    return NativeContractBuilder::host_fn_error_conversion(r2.error());
  }
  return *r2 == SyscallErrorCode::kSyscallErrorCodeNone;
}

// For now, the typedefs exported by a contract aren't added to the generated
// spec, unfortunately
using amount_t = std::tuple<uint64_t, uint64_t>;
using difference_t = std::tuple<int64_t, uint64_t>;

constexpr uint128_t kMaxInt128 = std::numeric_limits<int128_t>::max();

// implementation functions
InvokeStatus<Result<void, ERC20Exception>>
transfer_impl(NativeSyscallContext &ctx, ContractAddress receiver,
              amount_t amount)
{
  trace("native token transfer: receiver {} amount {}", x23::show(receiver),
        x23::show(amount));
  if (!ctx.consume_gas(1)) {
    return InvokeError{InvokeErrorCode::OutOfGas};
  }

  // For now, amount_t is a uint, so we do not need to check for negative
  // amounts here. adding the assertion in case that changes
  uint128_t amount_unsigned_int =
      uint128_t{std::get<0>(amount)} << 64 | std::get<1>(amount);

  auto owner = get_msg_sender(ctx);
  if (!owner.has_value()) {
    return owner.error();
  }

  // can't negate bigger values
  if (amount_unsigned_int > kMaxInt128) {
    return InvokeError{InvokeErrorCode::Deterministic};
  }
  int128_t amount_signed_int = amount_unsigned_int;

  // subtract amount from owner's balance
  auto r1 =
      ctx.handler().modify_native_token_balance(*owner, -amount_signed_int);
  if (!r1.has_value()) {
    return r1.error().to_invoke_error();
  }
  if (!r1.value().has_value()) {
    return InvokeError{InvokeErrorCode::Deterministic};
  }

  // add amount to receiver's balance
  auto r2 =
      ctx.handler().modify_native_token_balance(receiver, amount_signed_int);
  if (!r2.has_value()) {
    return r2.error().to_invoke_error();
  }
  if (!r2.value().has_value()) {
    return InvokeError{InvokeErrorCode::Deterministic};
  }

  auto event_buf = make_event_from_xdr_struct(TransferEvent{
      .from_address = *owner,
      .to_address = receiver,
      .amount = to_xdr(amount_signed_int),
  });

  if (auto err = emit_event<3>(ctx, event_buf); !err.has_value()) {
    warn("failed to log transfer event in native token transfer");
    return err.error();
  }

  return {Result<void, ERC20Exception>::make_ok()};
}

InvokeStatus<Result<void, ERC20Exception>>
transfer_from_impl(NativeSyscallContext &ctx, ContractAddress owner,
                   ContractAddress receiver, amount_t amount)
{
  trace("native token transfer_from: owner {} receiver {} amount {}",
        x23::show(owner), x23::show(receiver), x23::show(amount));
  if (!ctx.consume_gas(1)) {
    return InvokeError{InvokeErrorCode::OutOfGas};
  }

  uint128_t amount_unsigned_int =
      uint128_t{std::get<0>(amount)} << 64 | std::get<1>(amount);

  // get spender
  auto spender = get_msg_sender(ctx);
  if (!spender.has_value()) {
    return spender.error();
  }

  // can't negate bigger values
  if (amount_unsigned_int > kMaxInt128) {
    return InvokeError{InvokeErrorCode::Deterministic};
  }
  int128_t amount_signed_int = amount_unsigned_int;

  // subtract amount from owner's balance
  auto r1 =
      ctx.handler().modify_native_token_balance(owner, -amount_signed_int);
  if (!r1.has_value()) {
    return r1.error().to_invoke_error();
  }
  if (!r1.value()) {
    return InvokeError{InvokeErrorCode::Deterministic};
  }

  // add amount to receiver's balance
  auto r2 =
      ctx.handler().modify_native_token_balance(receiver, amount_signed_int);
  if (!r2.has_value()) {
    return r2.error().to_invoke_error();
  }
  if (!r2.value().has_value()) {
    return InvokeError{InvokeErrorCode::Deterministic};
  }

  if (owner != *spender) {
    auto allow_res = adjust_allowance(ctx, owner, *spender, -amount_signed_int);
    if (!allow_res.has_value()) {
      return allow_res.error();
    }
    if (!allow_res.value()) {
      return InvokeError{InvokeErrorCode::Deterministic};
    }
  }

  auto event_buf = make_event_from_xdr_struct(TransferEvent{
      .from_address = owner,
      .to_address = receiver,
      .amount = to_xdr(amount_signed_int),
  });

  if (auto err = emit_event<3>(ctx, event_buf); !err.has_value()) {
    warn("failed to log transfer_from event in native token transfer_from");
    return err.error();
  }

  return {Result<void, ERC20Exception>::make_ok()};
}

InvokeStatus<Result<void, ERC20Exception>>
adjust_allowance_impl(NativeSyscallContext &ctx, ContractAddress spender,
                      difference_t amount)
{
  trace("native token adjust_allowance: spender {} amount {}",
        x23::show(spender), x23::show(amount));
  if (!ctx.consume_gas(1)) {
    return InvokeError{InvokeErrorCode::OutOfGas};
  }

  auto owner = get_msg_sender(ctx);
  if (!owner.has_value()) {
    return owner.error();
  }

  if (*owner == spender) {
    return InvokeError{InvokeErrorCode::Deterministic};
  }

  int128_t amount_signed_int =
      int128_t{std::get<0>(amount)} << 64 | std::get<1>(amount);

  auto r = adjust_allowance(ctx, *owner, spender, amount_signed_int);
  if (!r.has_value()) {
    return r.error();
  }
  if (!r.value()) {
    return InvokeError{InvokeErrorCode::Deterministic};
  }

  auto event_buf = make_event_from_xdr_struct(ApprovalEvent{
      .owner_address = *owner,
      .spender_address = spender,
      .amount = to_xdr(amount_signed_int),
  });

  if (auto err = emit_event<3>(ctx, event_buf); !err.has_value()) {
    warn("failed to log adjust_allowance event in native token "
         "adjust_allowance");
    return err.error();
  }

  return {Result<void, ERC20Exception>::make_ok()};
}

InvokeStatus<Result<amount_t, ERC20Exception>>
get_allowance_impl(NativeSyscallContext &ctx, ContractAddress owner,
                   ContractAddress spender)
{
  trace("native token get_allowance: owner {} spender {}", x23::show(owner),
        x23::show(spender));
  if (!ctx.consume_gas(1)) {
    return InvokeError{InvokeErrorCode::OutOfGas};
  }

  auto allow_key = allowance_key(owner, spender);

  int128_t output_set{};
  bool output_set_opt{};
  int128_t output_add{};
  auto r =
      ctx.syscall<SyscallNo::kNonnegativeIntGet>(allow_key, true, output_set,
                                                 output_set_opt, output_add);
  if (!r.has_value()) {
    return NativeContractBuilder::host_fn_error_conversion(r.error());
  }
  if (*r == SyscallErrorCode::kSyscallErrorCodeStorageKeyNotExist) {
    return {Result<amount_t, ERC20Exception>::make_ok(
        std::make_tuple(uint64_t{0}, uint64_t{0}))};
  }
  if (*r != SyscallErrorCode::kSyscallErrorCodeNone) {
    return InvokeError{InvokeErrorCode::Deterministic};
  }

  CHECK(output_set_opt);
  auto [total, of] = overflowing_add(output_set, output_add);
  if (of) {
    return InvokeError{InvokeErrorCode::Deterministic};
  }
  uint128_t utotal = std::max<int128_t>(total, 0);
  return {Result<amount_t, ERC20Exception>::make_ok(
      std::make_tuple(utotal >> 64, utotal & 0xffffffffffffffffULL))};
}

} // namespace

std::unique_ptr<NativeContract>
make_native_token_contract()
{
  NativeContractBuilder builder;

  // initialize
  builder.add<itc_erc20::ERC20, "initialize">(
      kAlwaysErrorMethod<itc_erc20::ERC20, "initialize", 1>);

  // transfer
  builder.add<itc_erc20::ERC20, "transfer">(transfer_impl);

  // transfer_from
  builder.add<itc_erc20::ERC20, "transfer_from">(transfer_from_impl);

  // allowance
  builder.add<itc_erc20::ERC20, "allowance">(get_allowance_impl);

  // adjust_allowance
  builder.add<itc_erc20::ERC20, "adjust_allowance">(adjust_allowance_impl);

  // get_balance_of
  builder.add<itc_erc20::ERC20, "get_balance_of">(
      [](NativeSyscallContext &ctx, ContractAddress contract_address)
          -> InvokeStatus<Result<amount_t, ERC20Exception>> {
        trace("native token get_balance_of: contract_address {}",
              x23::show(contract_address));
        if (!ctx.consume_gas(1)) {
          return InvokeError{InvokeErrorCode::OutOfGas};
        }
        auto balance = ctx.handler().get_native_token_balance(contract_address);
        if (!balance.has_value()) {
          return balance.error().to_invoke_error();
        }
        if (!balance.value().has_value()) {
          return InvokeError{InvokeErrorCode::Deterministic};
        }
        assert(balance.value().value() >= 0);
        uint128_t ubalance = balance.value().value();
        return {Result<amount_t, ERC20Exception>::make_ok(
            std::make_tuple(ubalance >> 64, ubalance & 0xffffffffffffffffULL))};
      });

  // get_balance
  builder.add<itc_erc20::ERC20, "get_balance">(
      [](NativeSyscallContext &ctx)
          -> InvokeStatus<Result<amount_t, ERC20Exception>> {
        trace("native token get_balance");
        if (!ctx.consume_gas(1)) {
          return InvokeError{InvokeErrorCode::OutOfGas};
        }

        auto caller_address = get_msg_sender(ctx);
        if (!caller_address.has_value()) {
          return caller_address.error();
        }

        auto balance = ctx.handler().get_native_token_balance(*caller_address);
        if (!balance.has_value()) {
          return balance.error().to_invoke_error();
        }
        if (!balance.value().has_value()) {
          return InvokeError{InvokeErrorCode::Deterministic};
        }
        assert(balance.value().value() >= 0);
        uint128_t ubalance = balance.value().value();
        return {Result<amount_t, ERC20Exception>::make_ok(
            std::make_tuple(ubalance >> 64, ubalance & 0xffffffffffffffffULL))};
      });

  // mint
  builder.add<itc_erc20::ERC20, "mint">(
      kAlwaysErrorMethod<itc_erc20::ERC20, "mint", 1>);

  // burn
  builder.add<itc_erc20::ERC20, "burn">(
      kAlwaysErrorMethod<itc_erc20::ERC20, "burn", 1>);

  // total_supply
  builder.add<itc_erc20::ERC20, "total_supply">(
      [](NativeSyscallContext &runtime)
          -> InvokeStatus<Result<amount_t, ERC20Exception>> {
        trace("native token total_supply");
        // TODO(geoff): gas costs
        if (!runtime.consume_gas(1)) {
          return InvokeError{InvokeErrorCode::OutOfGas};
        }
        uint128_t total_supply =
            uint128_t{kTotalHogSupply} * uint128_t{kHogDivisibility};
        return {Result<amount_t, ERC20Exception>::make_ok(
            std::make_tuple(total_supply >> 64,
                            total_supply & 0xffffffffffffffffULL))};
      });

  assert(builder.check_methods<itc_erc20::ERC20>());

  return builder.build();
}

} // namespace hogd
