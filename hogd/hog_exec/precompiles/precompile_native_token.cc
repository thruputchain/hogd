#include "hogd/hog_exec/precompiles/precompile_native_token.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstring>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>

#include "auth_token.hh"
#include "built_contracts/itc_erc20.h"
#include "built_contracts/itc_erc20.hh"
#include "built_contracts/itc_erc20_permit.h"
#include "built_contracts/itc_erc20_permit.hh"
#include "event.hh"
#include "ffi_enums.hh"
#include "hogd/base/compiler.h"
#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h"
#include "hogd/crypto/crypto.h"
#include "hogd/genesis/genesis_constants.h"
#include "hogd/hog_event/utilities.h"
#include "hogd/hog_exec/common/status.h"
#include "hogd/hog_exec/components/contract_result.h"
#include "hogd/hog_exec/components/library_exception.hh"
#include "hogd/hog_exec/precompiles/native_contract_builder.h"
#include "hogd/hog_exec/runtime/contract_trait.h"
#include "hogd/hog_exec/runtime/native_runtime.h"
#include "hogd/hog_exec/runtime/native_syscall_context.h"
#include "hogd/hog_exec/runtime/serialize_calldata.h"
#include "standard_asset_events.hh"
#include "storage_item.hh"
#include "syscall.hh"
#include "syscall_error.hh"
#include "types.hh"
#include "x23/async.h"
#include "x23/format.h"
#include "x23/serialize_cxs.h"
#include "x23/vec.h"

namespace hogd {

using ERC20Exception =
    ContractException<itc_erc20::ERC20Error, 0xf63c8b1f5e7e2fe0ULL>;

namespace {

// Compute an event-signature topic the same way the SDK does:
// sha256("<major_version>::<crate_name>::<type_name>"). See
// groundhog_contract_util::event_signature in the SDK.
//
// The preimage is taken from a `u8"..."` literal so its bytes are the UTF-8
// (== ASCII, for these names) encoding mandated by the standard, independent of
// the ordinary execution character set (`-fexec-charset`). An ordinary `"..."`
// literal would hash implementation-defined code units, so two honest
// validators built with different execution charsets could emit divergent event
// topics and thus divergent block hashes. See hogd issue #2573.
Topic
compute_event_signature(std::u8string_view qualified_name)
{
  SHA256Hasher hasher;
  hasher.update(std::as_bytes(std::span(qualified_name)));
  SHA256Hash digest = hasher.finalize();
  Topic topic;
  static_assert(sizeof(topic) == sizeof(digest));
  std::memcpy(topic.data(), digest.data(), topic.size());
  return topic;
}

// Emit an event through the kLogEvent syscall.
// The event_buf must contain topics concatenated with data (as produced by
// make_event_from_cxs_struct). num_topics topics of sizeof(Topic) bytes each
// are split off the front; the remainder is data.
//
// event_name identifies the calling entry point in the log lines below.
template<uint32_t NumTopics>
x23::Async<InvokeStatus<void>>
emit_event(NativeSyscallContext &ctx, x23::Buf const &event_buf,
           std::string_view event_name)
{
  static_assert(NumTopics <= 3);
  auto buf = std::as_bytes(std::span(event_buf));

  // NumTopics must match the event struct's leading fields; a mismatch is a
  // build bug.
  CHECK_GE(buf.size(), NumTopics * sizeof(Topic));

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
  auto r = co_await ctx.syscall<SyscallNo::kLogEvent>(t0, t1, t2, data);
  if (!r.has_value()) {
    // OutOfGas means this frame could not even pay kSyscallBaseCost for the
    // crossing; the remaining codes abandon the transaction outright.
    info("native token {} event: kLogEvent failed: {} ({} topics, {} data "
         "bytes, {} gas left)",
         event_name, r.error(), NumTopics, data.size(), ctx.gas_remaining());
    co_return NativeContractBuilder::host_fn_error_conversion(r.error());
  }
  if (*r != SyscallErrorCode::kSyscallErrorCodeNone) {
    // log_event only rejects oversized events or the per-tx event limit.
    info("native token {} event: kLogEvent rejected the event: {} ({} topics, "
         "{} data bytes)",
         event_name, x23::show(*r), NumTopics, data.size());
    co_return InvokeError{InvokeErrorCode::Deterministic};
  }
  co_return InvokeStatus<void>{};
}

// Get the msg_sender via the kGetMsgSender syscall.
x23::Async<InvokeStatus<ContractAddress>>
get_msg_sender(NativeSyscallContext &ctx)
{
  ContractAddress sender{};
  auto r = co_await ctx.syscall<SyscallNo::kGetMsgSender>(sender);
  if (!r.has_value()) {
    co_return NativeContractBuilder::host_fn_error_conversion(r.error());
  }
  if (*r != SyscallErrorCode::kSyscallErrorCodeNone) {
    co_return InvokeError{InvokeErrorCode::Deterministic};
  }
  co_return sender;
}

// Derive the allowance storage key; routed through kSHA256 so the digest is
// priced as a syscall (issue #2963).
x23::Async<InvokeStatus<StorageKeyContractData>>
allowance_key(NativeSyscallContext &ctx, ContractAddress owner,
              ContractAddress spender)
{
  auto data = Serializer<SerializationFormat::kTxs>::serialize(owner, spender);
  if (!data.has_value()) {
    // Two fixed-size addresses cannot fail to serialize; if they do, the host
    // is broken, not the contract.
    error("Unrecoverable: could not serialize the allowance key");
    co_return InvokeError{InvokeErrorCode::Unrecoverable};
  }

  SHA256Hash key{};
  auto r = co_await ctx.syscall<SyscallNo::kSHA256>(
      std::as_bytes(std::span(data->data(), data->size())), key);
  if (!r.has_value()) {
    // OutOfGas means the frame cannot pay the crossing plus the digest; the
    // remaining codes abandon the transaction outright.
    co_return NativeContractBuilder::host_fn_error_conversion(r.error());
  }
  if (*r != SyscallErrorCode::kSyscallErrorCodeNone) {
    // kSHA256 documents no error codes, so this is a host fault rather than
    // anything the caller did. Unlike get_msg_sender above, whose
    // kSyscallErrorCodeIsRootTransaction is reachable and is the contract's.
    error("Unrecoverable: kSHA256 returned {}", x23::show(*r));
    co_return InvokeError{InvokeErrorCode::Unrecoverable};
  }
  co_return key;
}

// Return an ERC20 contract-level revert carrying `code`, byte-identical to the
// reference contract throwing the same ERC20Error (same selector + CXS body).
x23::Async<InvokeStatus<Result<void, ERC20Exception>>>
erc20_error(itc_erc20::ERC20Error code)
{
  co_return Result<void, ERC20Exception>::make_err(ERC20Exception{code});
}

// Strict allowance adjustment, mirroring itc_erc20::add_to_allowance (the SDK's
// non_negative_int_try_strict_inc): force-load the committed allowance and
// reject an out-of-range running total at invoke (returns false), rather than
// deferring to commit. The balance debit in transfer_impl / transfer_from_impl
// mirrors the same SDK primitive.
x23::Async<InvokeStatus<bool>>
adjust_allowance(NativeSyscallContext &ctx, ContractAddress owner,
                 ContractAddress spender, int128_t amount)
{
  auto allow_key_r = co_await allowance_key(ctx, owner, spender);
  if (!allow_key_r.has_value()) {
    co_return allow_key_r.error();
  }
  auto const &allow_key = *allow_key_r;

  int128_t output_set{};
  bool output_set_opt{};
  int128_t output_add{};
  auto r = co_await ctx.syscall<SyscallNo::kNonnegativeIntGet>(
      allow_key,
      /*load_from_snapshot=*/true, output_set, output_set_opt, output_add);
  if (!r.has_value()) {
    co_return NativeContractBuilder::host_fn_error_conversion(r.error());
  }
  if (*r == SyscallErrorCode::kSyscallErrorCodeStorageKeyNotExist) {
    // Never-written allowance: treat as an explicit zero, matching
    // try_get_values_force().unwrap_or((0, 0)) + set_add(Some(0), ...).
    output_set = 0;
    output_set_opt = true;
    output_add = 0;
  }
  else if (*r != SyscallErrorCode::kSyscallErrorCodeNone) {
    // An unexpected syscall code (e.g. a type mismatch on the allowance key) is
    // a storage anomaly, not an insufficient allowance: surface it as a
    // deterministic fault rather than letting callers report ERC20Error::
    // TooLarge, matching get_allowance_impl.
    co_return InvokeError{InvokeErrorCode::Deterministic};
  }

  if (integer_add_overflow(output_add, amount)) {
    co_return false;
  }
  int128_t new_add = output_add + amount;
  // Strict non-negativity on the committed base + net in-tx delta.
  if (integer_add_overflow(output_set, new_add) || (output_set + new_add) < 0) {
    co_return false;
  }

  const int128_t *set_ptr = output_set_opt ? &output_set : nullptr;
  auto r2 =
      co_await ctx.syscall<SyscallNo::kNonnegativeIntSetAdd>(allow_key, set_ptr,
                                                             new_add);
  if (!r2.has_value()) {
    co_return NativeContractBuilder::host_fn_error_conversion(r2.error());
  }
  if (*r2 != SyscallErrorCode::kSyscallErrorCodeNone) {
    co_return false;
  }
  co_return true;
}

// For now, the typedefs exported by a contract aren't added to the generated
// spec, unfortunately. Amount and Difference are both i128; every entry point
// taking an Amount rejects a negative one with ERC20Error::NegativeAmount, as
// the reference's check_amount does.
using amount_t = std::tuple<int64_t, uint64_t>;
using difference_t = std::tuple<int64_t, uint64_t>;

int128_t
from_wire(std::tuple<int64_t, uint64_t> value)
{
  return int128_t{std::get<0>(value)} << 64 | std::get<1>(value);
}

// Balances, allowances and supply are nonnegative by the time they are
// returned; callers fault a negative reading before getting here.
amount_t
to_amount(int128_t value)
{
  CHECK(value >= 0);
  return {static_cast<int64_t>(value >> 64),
          static_cast<uint64_t>(value & 0xffffffffffffffffULL)};
}

using MintStatus = InvokeStatus<Result<void, ERC20Exception>>;

// Mirror the reference's add_to_total_supply (the SDK's plain, non-strict
// non_negative_int_inc): combine the increment with the in-transaction values
// only -- no snapshot force-load, and no invoke-time range check against the
// committed base, which is validated at commit instead. The one invoke-time
// failure is an overflow of the accumulated in-tx add, which the SDK reports
// by throwing LibraryExn::NonNegativeIntOverflow -- returned here as the
// byte-identical library abort. Returns nullopt once the increment is staged.
x23::Async<std::optional<MintStatus>>
add_to_total_supply(NativeSyscallContext &ctx, int128_t diff)
{
  int128_t set_value{};
  bool set_value_present{};
  int128_t add_value{};
  auto get = co_await ctx.syscall<SyscallNo::kNonnegativeIntGet>(
      kNativeTokenTotalSupplyKey,
      /*load_from_snapshot=*/false, set_value, set_value_present, add_value);
  if (!get) {
    co_return MintStatus{
        NativeContractBuilder::host_fn_error_conversion(get.error())};
  }
  if (*get != SyscallErrorCode::kSyscallErrorCodeNone) {
    co_return MintStatus{InvokeError{InvokeErrorCode::Deterministic}};
  }
  if (integer_add_overflow(add_value, diff)) {
    co_return MintStatus{
        {Result<void, ERC20Exception>::make_err(LibraryException{
            LibraryExn(LibraryExnKind::kLibraryExnNonNegativeIntOverflow)})}};
  }
  const int128_t *set_ptr = set_value_present ? &set_value : nullptr;
  auto set = co_await ctx.syscall<SyscallNo::kNonnegativeIntSetAdd>(
      kNativeTokenTotalSupplyKey, set_ptr, add_value + diff);
  if (!set) {
    co_return MintStatus{
        NativeContractBuilder::host_fn_error_conversion(set.error())};
  }
  if (*set != SyscallErrorCode::kSyscallErrorCodeNone) {
    co_return MintStatus{InvokeError{InvokeErrorCode::Deterministic}};
  }
  co_return std::nullopt;
}

x23::Async<InvokeStatus<Result<void, ERC20Exception>>>
mint_impl(NativeSyscallContext &ctx, ContractAddress receiver, amount_t amount)
{
  if (!ctx.consume_gas(1)) {
    co_return InvokeError{InvokeErrorCode::OutOfGas};
  }
  auto sender = co_await get_msg_sender(ctx);
  if (!sender) {
    co_return sender.error();
  }
  // The genesis root plays the reference's stored contract owner; any other
  // caller gets the reference's rejection, byte-identically.
  if (*sender != kGenesisRootContractAddress) {
    co_return co_await erc20_error(itc_erc20::ERC20Error::MustBeContractOwner);
  }

  const int128_t signed_amount = from_wire(amount);
  if (signed_amount < 0) {
    co_return co_await erc20_error(itc_erc20::ERC20Error::NegativeAmount);
  }

  // Strict credit, mirroring the reference's add_to_balance: reject at
  // invoke with TooLarge when the running balance (committed base + net
  // in-tx delta) cannot absorb the credit, before staging any state.
  auto receiver_balance = co_await ctx.native_token_balance(receiver);
  if (!receiver_balance) {
    co_return receiver_balance.error().to_invoke_error();
  }
  if (!receiver_balance.value().has_value()) {
    // Staged-negative balance (system-path state the RV32 reference can never
    // occupy) surfaces as a deterministic fault, not TooLarge -- deliberate
    // carve-out from byte parity, as in transfer_impl.
    co_return InvokeError{InvokeErrorCode::Deterministic};
  }
  if (integer_add_overflow(receiver_balance.value().value(), signed_amount)) {
    co_return co_await erc20_error(itc_erc20::ERC20Error::TooLarge);
  }

  auto credit =
      co_await ctx.modify_native_token_balance(receiver, signed_amount);
  if (!credit) {
    co_return credit.error().to_invoke_error();
  }
  if (!credit.value().has_value()) {
    co_return InvokeError{InvokeErrorCode::Deterministic};
  }

  // Reference order: the balance credit first, then the deferred supply add.
  if (auto supply_failure = co_await add_to_total_supply(ctx, signed_amount)) {
    co_return std::move(*supply_failure);
  }

  const NativeTokenMintEvent event{.to_address = receiver,
                                   .amount = to_xdr(signed_amount)};
  auto event_buf =
      make_event_from_cxs_struct(native_token_mint_event_signature(), event);
  if (!event_buf) {
    co_return InvokeError{InvokeErrorCode::Deterministic};
  }
  if (auto emitted = co_await emit_event<2>(ctx, *event_buf, "mint");
      !emitted) {
    co_return emitted.error();
  }
  co_return Result<void, ERC20Exception>::make_ok();
}

// The owner-to-receiver leg every spending path shares: strict debit, credit,
// Transfer event. `amount` is already range-checked and the caller authorized;
// ResultT/Erc20ExceptionT are the calling interface's Result and ERC20Error.
template<typename ResultT, typename Erc20ExceptionT>
x23::Async<InvokeStatus<ResultT>>
move_balance(NativeSyscallContext &ctx, ContractAddress const &owner,
             ContractAddress const &receiver, int128_t amount,
             std::string_view entry_point)
{
  using erc20_error_t = decltype(Erc20ExceptionT::exn);

  // Strict per-debit check mirroring itc_erc20's add_to_balance /
  // non_negative_int_try_strict_inc: the running balance (committed base +
  // net in-tx delta) may not go below zero. A debit landing exactly at
  // zero is accepted -- the reference's only rejection is `total < 0`.
  auto owner_avail = co_await ctx.native_token_balance(owner);
  if (!owner_avail.has_value()) {
    co_return owner_avail.error().to_invoke_error();
  }
  if (!owner_avail.value().has_value()) {
    // Staged-negative balance (system-path state the RV32 reference can never
    // occupy) surfaces as a deterministic fault, not TooLarge -- deliberate
    // carve-out from byte parity.
    co_return InvokeError{InvokeErrorCode::Deterministic};
  }
  if (amount > owner_avail.value().value()) {
    co_return ResultT::make_err(Erc20ExceptionT{erc20_error_t::TooLarge});
  }

  // No credit-side overflow check: the receiver balance and [amount] are both
  // part of total supply, which never exceeds INT128_MAX, so their sum cannot
  // overflow and the reference's checked add on the credit leg cannot fire.
  // HogDb's commit-time add-capacity bound remains the backstop for
  // hand-crafted states.

  if (auto moved = co_await native_token_unchecked_transfer(ctx, owner,
                                                            receiver, amount);
      !moved.has_value()) {
    co_return moved.error();
  }

  auto event_buf =
      make_event_from_cxs_struct(native_token_transfer_event_signature(),
                                 TransferEvent{
                                     .from_address = owner,
                                     .to_address = receiver,
                                     .amount = to_xdr(amount),
                                 });
  if (!event_buf)
    co_return InvokeError{InvokeErrorCode::Deterministic};

  if (auto err = co_await emit_event<3>(ctx, *event_buf, entry_point);
      !err.has_value()) {
    co_return err.error();
  }

  co_return ResultT::make_ok();
}

// implementation functions
x23::Async<InvokeStatus<Result<void, ERC20Exception>>>
transfer_impl(NativeSyscallContext &ctx, ContractAddress receiver,
              amount_t amount)
{
  trace("native token transfer: receiver {} amount {}", x23::show(receiver),
        x23::show(amount));
  if (!ctx.consume_gas(1)) {
    co_return InvokeError{InvokeErrorCode::OutOfGas};
  }

  auto owner = co_await get_msg_sender(ctx);
  if (!owner.has_value()) {
    co_return owner.error();
  }

  const int128_t amount_signed_int = from_wire(amount);
  if (amount_signed_int < 0) {
    co_return co_await erc20_error(itc_erc20::ERC20Error::NegativeAmount);
  }

  co_return co_await move_balance<Result<void, ERC20Exception>, ERC20Exception>(
      ctx, *owner, receiver, amount_signed_int, "transfer");
}

x23::Async<InvokeStatus<Result<void, ERC20Exception>>>
transfer_from_impl(NativeSyscallContext &ctx, ContractAddress owner,
                   ContractAddress receiver, amount_t amount)
{
  trace("native token transfer_from: owner {} receiver {} amount {}",
        x23::show(owner), x23::show(receiver), x23::show(amount));
  if (!ctx.consume_gas(1)) {
    co_return InvokeError{InvokeErrorCode::OutOfGas};
  }

  // get spender
  auto spender = co_await get_msg_sender(ctx);
  if (!spender.has_value()) {
    co_return spender.error();
  }

  const int128_t amount_signed_int = from_wire(amount);
  if (amount_signed_int < 0) {
    co_return co_await erc20_error(itc_erc20::ERC20Error::NegativeAmount);
  }

  // Order mirrors itc_erc20::transfer_from: the (strict) allowance debit
  // first when spending someone else's tokens, then the strict owner
  // debit, then the receiver credit. Any failure aborts the frame and
  // reverts the staged allowance debit.
  if (owner != *spender) {
    auto allow_res =
        co_await adjust_allowance(ctx, owner, *spender, -amount_signed_int);
    if (!allow_res.has_value()) {
      co_return allow_res.error();
    }
    if (!allow_res.value()) {
      co_return co_await erc20_error(itc_erc20::ERC20Error::TooLarge);
    }
  }

  co_return co_await move_balance<Result<void, ERC20Exception>, ERC20Exception>(
      ctx, owner, receiver, amount_signed_int, "transfer_from");
}

x23::Async<InvokeStatus<void>>
emit_allowance_change(NativeSyscallContext &ctx, ContractAddress const &owner,
                      ContractAddress const &spender, int128_t difference)
{
  auto event = make_event_from_cxs_struct(
      native_token_allowance_change_event_signature(),
      NativeTokenAllowanceChangeEvent{.owner_address = owner,
                                      .spender_address = spender,
                                      .difference = to_xdr(difference)});
  if (!event) {
    co_return InvokeError{InvokeErrorCode::Deterministic};
  }
  co_return co_await emit_event<3>(ctx, *event, "allowance change");
}

x23::Async<InvokeStatus<Result<void, ERC20Exception>>>
adjust_allowance_impl(NativeSyscallContext &ctx, ContractAddress spender,
                      difference_t amount)
{
  trace("native token adjust_allowance: spender {} amount {}",
        x23::show(spender), x23::show(amount));
  if (!ctx.consume_gas(1)) {
    co_return InvokeError{InvokeErrorCode::OutOfGas};
  }

  auto owner = co_await get_msg_sender(ctx);
  if (!owner.has_value()) {
    co_return owner.error();
  }

  const int128_t amount_signed_int = from_wire(amount);

  auto r = co_await adjust_allowance(ctx, *owner, spender, amount_signed_int);
  if (!r.has_value()) {
    co_return r.error();
  }
  if (!r.value()) {
    co_return co_await erc20_error(itc_erc20::ERC20Error::TooLarge);
  }

  if (auto event = co_await emit_allowance_change(ctx, *owner, spender,
                                                  amount_signed_int);
      !event) {
    co_return event.error();
  }

  co_return Result<void, ERC20Exception>::make_ok();
}

x23::Async<InvokeStatus<int128_t>>
read_allowance(NativeSyscallContext &ctx, StorageKeyContractData const &key)
{
  int128_t base{};
  bool base_present{};
  int128_t delta{};
  auto read =
      co_await ctx.syscall<SyscallNo::kNonnegativeIntGet>(key, true, base,
                                                          base_present, delta);
  if (!read) {
    co_return NativeContractBuilder::host_fn_error_conversion(read.error());
  }
  if (*read == SyscallErrorCode::kSyscallErrorCodeStorageKeyNotExist) {
    co_return int128_t{0};
  }
  if (*read != SyscallErrorCode::kSyscallErrorCodeNone || !base_present ||
      integer_add_overflow(base, delta) || base + delta < 0) {
    co_return InvokeError{InvokeErrorCode::Deterministic};
  }
  co_return base + delta;
}

x23::Async<InvokeStatus<Result<amount_t, ERC20Exception>>>
get_allowance_impl(NativeSyscallContext &ctx, ContractAddress owner,
                   ContractAddress spender)
{
  if (!ctx.consume_gas(1)) {
    co_return InvokeError{InvokeErrorCode::OutOfGas};
  }
  auto key = co_await allowance_key(ctx, owner, spender);
  if (!key) {
    co_return key.error();
  }
  auto remaining = co_await read_allowance(ctx, *key);
  if (!remaining) {
    co_return remaining.error();
  }
  co_return Result<amount_t, ERC20Exception>::make_ok(to_amount(*remaining));
}

// Delete the allowance under an exclusive lock and emit AllowanceDeleteEvent
// with the amount removed, as the reference's delete_allowance does.
x23::Async<InvokeStatus<void>>
delete_allowance(NativeSyscallContext &ctx, ContractAddress const &owner,
                 ContractAddress const &spender)
{
  auto key = co_await allowance_key(ctx, owner, spender);
  if (!key) {
    co_return key.error();
  }
  auto remaining = co_await read_allowance(ctx, *key);
  if (!remaining) {
    co_return remaining.error();
  }
  auto deleted = co_await ctx.syscall<SyscallNo::kDeleteObject>(*key);
  if (!deleted) {
    co_return NativeContractBuilder::host_fn_error_conversion(deleted.error());
  }
  if (*deleted != SyscallErrorCode::kSyscallErrorCodeNone) {
    co_return InvokeError{InvokeErrorCode::Deterministic};
  }
  auto locked = co_await ctx.syscall<SyscallNo::kSetStorageFlags>(
      *key, static_cast<StorageDeltaFlagSet>(StorageDeltaFlags::ExclusiveLock));
  if (!locked) {
    co_return NativeContractBuilder::host_fn_error_conversion(locked.error());
  }
  if (*locked != SyscallErrorCode::kSyscallErrorCodeNone) {
    error(
        "Unrecoverable: kSetStorageFlags after allowance deletion returned {}",
        x23::show(*locked));
    co_return InvokeError{InvokeErrorCode::Unrecoverable};
  }
  auto event = make_event_from_cxs_struct(
      native_token_allowance_delete_event_signature(),
      NativeTokenAllowanceDeleteEvent{.owner_address = owner,
                                      .spender_address = spender,
                                      .removed = to_xdr(*remaining)});
  if (!event) {
    co_return InvokeError{InvokeErrorCode::Deterministic};
  }
  co_return co_await emit_event<3>(ctx, *event, "allowance delete");
}

x23::Async<InvokeStatus<Result<void, ERC20Exception>>>
delete_allowance_impl(NativeSyscallContext &ctx, ContractAddress spender)
{
  if (!ctx.consume_gas(1)) {
    co_return InvokeError{InvokeErrorCode::OutOfGas};
  }
  auto owner = co_await get_msg_sender(ctx);
  if (!owner) {
    co_return owner.error();
  }
  if (auto deleted = co_await delete_allowance(ctx, *owner, spender);
      !deleted) {
    co_return deleted.error();
  }
  co_return Result<void, ERC20Exception>::make_ok();
}

// ERC20Permit (thruput-erc20-permit-if): approvals that arrive as auth
// tokens rather than as a transaction from the owner. The RV32 reference is
// itc_erc20_permit, which is itc_erc20 with this interface on top.

using PermitErc20Exception =
    ContractException<itc_erc20_permit::ERC20Error, 0xf63c8b1f5e7e2fe0ULL>;
using PermitException = ContractException<itc_erc20_permit::ERC20PermitError,
                                          0x1aaf2b9becb7ddc4ULL>;
// Error order follows the generated itc_erc20_permit header, which lists a
// method's exceptions in the spec's throws order (by selector).
using PermitResult = Result<void, PermitException, PermitErc20Exception>;

static_assert(ERC20Exception::kSelector == PermitErc20Exception::kSelector,
              "ERC20Error must reject identically through either interface");

// A consumed grant and the address that minted it: the owner it speaks for.
template<typename Grant> struct Consumed {
  Grant grant;
  ContractAddress owner;
};

template<typename Grant>
using ConsumeResult = std::variant<Consumed<Grant>, LibraryException>;

// Consume `token` as the SDK's `Signed<Grant>::consume` does, returning the
// grant or the library exception the SDK would throw in its place.
template<typename Grant>
x23::Async<InvokeStatus<ConsumeResult<Grant>>>
consume_grant(NativeSyscallContext &ctx, SignedToken const &token)
{
  auto auth_token_exception = [](AuthTokenErrorKind kind) {
    LibraryExn exn(LibraryExnKind::kLibraryExnAuthToken);
    exn.auth_token() = AuthTokenError(kind);
    return LibraryException{std::move(exn)};
  };

  std::array<std::byte, kAuthTokenMaxLen> payload{};
  ContractAddress minter{};
  RuntimeMemoryLength payload_len = 0;
  uint32_t child_start = 0;
  uint32_t child_count = 0;
  auto r = co_await ctx.syscall<SyscallNo::kConsumeAuthToken>(
      token.handle, std::span<std::byte>(payload), minter, payload_len,
      child_start, child_count);
  if (!r.has_value()) {
    co_return NativeContractBuilder::host_fn_error_conversion(r.error());
  }
  if (*r != SyscallErrorCode::kSyscallErrorCodeNone) {
    co_return ConsumeResult<Grant>{
        NativeContractBuilder::syscall_exception(SyscallNo::kConsumeAuthToken,
                                                 *r)};
  }
  if (payload_len > payload.size()) {
    // The host copies at most the buffer it was given; a longer length is a
    // host fault, not something the grant's minter can cause.
    error("Unrecoverable: kConsumeAuthToken reported {} payload bytes into a "
          "{}-byte buffer",
          payload_len, payload.size());
    co_return InvokeError{InvokeErrorCode::Unrecoverable};
  }

  Grant grant{};
  auto const *payload_bytes = reinterpret_cast<uint8_t const *>(payload.data());
  if (!x23::deserialize_cxs(&grant, std::span<uint8_t const>(payload_bytes,
                                                             payload_len))) {
    co_return ConsumeResult<Grant>{auth_token_exception(
        AuthTokenErrorKind::kAuthTokenConsumeDecodeFailure)};
  }
  // Grants have no Signed field to receive child tokens. The syscall has
  // unwrapped them into this frame either way, as it has for the reference.
  if (child_count != 0) {
    co_return ConsumeResult<Grant>{auth_token_exception(
        AuthTokenErrorKind::kAuthTokenReceiveAuthHandlesFailure)};
  }
  co_return ConsumeResult<Grant>{
      Consumed<Grant>{.grant = grant, .owner = minter}};
}

x23::Async<InvokeStatus<PermitResult>>
transfer_from_signed_impl(NativeSyscallContext &ctx, SignedToken grant_token,
                          ContractAddress receiver, amount_t amount)
{
  trace("native token transfer_from_signed: handle {} receiver {} amount {}",
        grant_token.handle, x23::show(receiver), x23::show(amount));
  if (!ctx.consume_gas(1)) {
    co_return InvokeError{InvokeErrorCode::OutOfGas};
  }

  auto consumed =
      co_await consume_grant<itc_erc20_permit::PermitGrant>(ctx, grant_token);
  if (!consumed.has_value()) {
    co_return consumed.error();
  }
  if (auto *exn = std::get_if<LibraryException>(&*consumed)) {
    co_return PermitResult::make_err(std::move(*exn));
  }
  auto const &[grant, owner] =
      std::get<Consumed<itc_erc20_permit::PermitGrant>>(*consumed);

  // The grant names who may spend it and how much; no allowance is written or
  // read, so this is the whole authorization check.
  auto caller = co_await get_msg_sender(ctx);
  if (!caller.has_value()) {
    co_return caller.error();
  }
  if (*caller != grant.spender) {
    co_return PermitResult::make_err(
        PermitException{itc_erc20_permit::ERC20PermitError::NotSpender});
  }
  const int128_t requested = from_wire(amount);
  if (requested > from_xdr(grant.amount)) {
    co_return PermitResult::make_err(
        PermitException{itc_erc20_permit::ERC20PermitError::ExceedsGrant});
  }
  // The reference checks the amount's sign only after the grant checks.
  if (requested < 0) {
    co_return PermitResult::make_err(
        PermitErc20Exception{itc_erc20_permit::ERC20Error::NegativeAmount});
  }

  co_return co_await move_balance<PermitResult, PermitErc20Exception>(
      ctx, owner, receiver, requested, "transfer_from_signed");
}

x23::Async<InvokeStatus<PermitResult>>
increase_allowance_signed_impl(NativeSyscallContext &ctx,
                               SignedToken grant_token)
{
  trace("native token increase_allowance_signed: handle {}",
        grant_token.handle);
  if (!ctx.consume_gas(1)) {
    co_return InvokeError{InvokeErrorCode::OutOfGas};
  }

  auto consumed =
      co_await consume_grant<itc_erc20_permit::PermitGrant>(ctx, grant_token);
  if (!consumed.has_value()) {
    co_return consumed.error();
  }
  if (auto *exn = std::get_if<LibraryException>(&*consumed)) {
    co_return PermitResult::make_err(std::move(*exn));
  }
  auto const &[grant, owner] =
      std::get<Consumed<itc_erc20_permit::PermitGrant>>(*consumed);

  const int128_t diff = from_xdr(grant.amount);
  if (diff < 0) {
    co_return PermitResult::make_err(
        PermitErc20Exception{itc_erc20_permit::ERC20Error::NegativeAmount});
  }
  // Adds to the allowance rather than setting it, as adjust_allowance does.
  auto adjusted = co_await adjust_allowance(ctx, owner, grant.spender, diff);
  if (!adjusted.has_value()) {
    co_return adjusted.error();
  }
  if (!adjusted.value()) {
    co_return PermitResult::make_err(
        PermitErc20Exception{itc_erc20_permit::ERC20Error::TooLarge});
  }

  if (auto err =
          co_await emit_allowance_change(ctx, owner, grant.spender, diff);
      !err.has_value()) {
    co_return err.error();
  }
  co_return PermitResult::make_ok();
}

x23::Async<InvokeStatus<PermitResult>>
delete_allowance_signed_impl(NativeSyscallContext &ctx, SignedToken grant_token)
{
  trace("native token delete_allowance_signed: handle {}", grant_token.handle);
  if (!ctx.consume_gas(1)) {
    co_return InvokeError{InvokeErrorCode::OutOfGas};
  }

  auto consumed =
      co_await consume_grant<itc_erc20_permit::DeleteAllowanceGrant>(
          ctx, grant_token);
  if (!consumed.has_value()) {
    co_return consumed.error();
  }
  if (auto *exn = std::get_if<LibraryException>(&*consumed)) {
    co_return PermitResult::make_err(std::move(*exn));
  }
  auto const &[grant, owner] =
      std::get<Consumed<itc_erc20_permit::DeleteAllowanceGrant>>(*consumed);

  if (auto deleted = co_await delete_allowance(ctx, owner, grant.spender);
      !deleted) {
    co_return deleted.error();
  }
  co_return PermitResult::make_ok();
}

// ERC165 (thruput-erc165-if). The RV32 reference answers from the interface
// table its #[contract] impls leave behind; the native token has no such
// table, so it lists the interfaces make_native_token_contract registers.

using erc165_meta = ContractTraitMeta<itc_erc20_permit::ERC165>;
using interface_ids_t =
    std::tuple_element_t<0, erc165_meta::args_t<u8"supports_interfaces">>;
using interface_support_t = erc165_meta::return_t<u8"supports_interfaces">;

static_assert(
    std::is_same_v<
        std::tuple_element_t<0, erc165_meta::args_t<u8"supports_interface">>,
        InterfaceId>,
    "ERC165 must take the InterfaceId sdk_interface_id_derivation returns");

template<is_contract_trait Trait>
InterfaceId
interface_id_of()
{
  return sdk_interface_id_derivation(Trait::kTraitName, Trait::kVersion);
}

bool
native_token_implements(InterfaceId const &id)
{
  // Keep in step with the builder.add calls in make_native_token_contract.
  static const std::array<InterfaceId, 3> kImplemented{
      interface_id_of<itc_erc20::ERC20>(),
      interface_id_of<itc_erc20_permit::ERC20Permit>(),
      interface_id_of<itc_erc20_permit::ERC165>(),
  };
  return std::ranges::contains(kImplemented, id);
}

// Ballparks of what the RV32 reference's frame consumes, from the runtime's
// "gas consumed" trace: supports_interface 470-533, and supports_interfaces
// about 2450 for an empty query.
constexpr GasAmount kSupportsInterfaceGas = 500;
constexpr GasAmount kSupportsInterfacesGas = 2500;

x23::Async<InvokeStatus<bool>>
supports_interface_impl(NativeSyscallContext &ctx, InterfaceId interface_id)
{
  if (!ctx.consume_gas(kSupportsInterfaceGas)) {
    co_return InvokeError{InvokeErrorCode::OutOfGas};
  }
  co_return native_token_implements(interface_id);
}

x23::Async<InvokeStatus<interface_support_t>>
supports_interfaces_impl(NativeSyscallContext &ctx,
                         interface_ids_t interface_ids)
{
  if (!ctx.consume_gas(kSupportsInterfacesGas)) {
    co_return InvokeError{InvokeErrorCode::OutOfGas};
  }
  // One answer per queried id, in order; the answer shares the query's bound.
  interface_support_t support;
  for (auto const &id : interface_ids) {
    support.push_back(native_token_implements(id));
  }
  co_return support;
}

} // namespace

x23::Async<InvokeStatus<void>>
native_token_unchecked_transfer(NativeSyscallContext &ctx,
                                ContractAddress const &from,
                                ContractAddress const &to, int128_t amount)
{
  // Callers are privileged system code (currently the strict trait methods);
  // a negative amount is a precondition violation, not a recoverable input.
  CHECK(amount >= 0);
  auto r1 = co_await ctx.modify_native_token_balance(from, -amount);
  if (!r1.has_value()) {
    co_return r1.error().to_invoke_error();
  }
  if (!r1.value().has_value()) {
    co_return InvokeError{InvokeErrorCode::Deterministic};
  }
  auto r2 = co_await ctx.modify_native_token_balance(to, amount);
  if (!r2.has_value()) {
    co_return r2.error().to_invoke_error();
  }
  if (!r2.value().has_value()) {
    co_return InvokeError{InvokeErrorCode::Deterministic};
  }
  co_return InvokeStatus<void>{};
}

Topic const &
native_token_transfer_event_signature()
{
  static const Topic kSignature =
      compute_event_signature(u8"0.1::thruput-erc20-if::TransferEvent");
  return kSignature;
}

Topic const &
native_token_allowance_change_event_signature()
{
  static const Topic kSignature =
      compute_event_signature(u8"0.1::thruput-erc20-if::AllowanceChangeEvent");
  return kSignature;
}

Topic const &
native_token_mint_event_signature()
{
  static const Topic kSignature =
      compute_event_signature(u8"0.1::thruput-erc20-if::MintEvent");
  return kSignature;
}

Topic const &
native_token_allowance_delete_event_signature()
{
  static const Topic kSignature =
      compute_event_signature(u8"0.1::thruput-erc20-if::AllowanceDeleteEvent");
  return kSignature;
}

std::unique_ptr<NativeContract>
make_native_token_contract()
{
  NativeContractBuilder builder;

  // initialize
  builder.add<itc_erc20::ERC20, u8"initialize">(
      kAlwaysErrorMethod<itc_erc20::ERC20, u8"initialize", 1>);

  // transfer
  builder.add<itc_erc20::ERC20, u8"transfer">(transfer_impl);

  // transfer_from
  builder.add<itc_erc20::ERC20, u8"transfer_from">(transfer_from_impl);

  // allowance
  builder.add<itc_erc20::ERC20, u8"allowance">(get_allowance_impl);

  // adjust_allowance
  builder.add<itc_erc20::ERC20, u8"adjust_allowance">(adjust_allowance_impl);

  builder.add<itc_erc20::ERC20, u8"delete_allowance">(delete_allowance_impl);

  // Native account balances also support storage accounting; their database
  // path does not support deletion or exclusive locks.
  builder.add<itc_erc20::ERC20, u8"delete_balance">(
      kAlwaysErrorMethod<itc_erc20::ERC20, u8"delete_balance", 1>);

  // get_balance_of
  builder.add<itc_erc20::ERC20, u8"get_balance_of">(
      [](NativeSyscallContext &ctx, ContractAddress contract_address)
          -> x23::Async<InvokeStatus<Result<amount_t, ERC20Exception>>> {
        trace("native token get_balance_of: contract_address {}",
              x23::show(contract_address));
        if (!ctx.consume_gas(1)) {
          co_return InvokeError{InvokeErrorCode::OutOfGas};
        }
        auto balance = co_await ctx.native_token_balance(contract_address);
        if (!balance.has_value()) {
          co_return balance.error().to_invoke_error();
        }
        if (!balance.value().has_value()) {
          co_return InvokeError{InvokeErrorCode::Deterministic};
        }
        co_return Result<amount_t, ERC20Exception>::make_ok(
            to_amount(balance.value().value()));
      });

  // get_balance
  builder.add<itc_erc20::ERC20, u8"get_balance">(
      [](NativeSyscallContext &ctx)
          -> x23::Async<InvokeStatus<Result<amount_t, ERC20Exception>>> {
        trace("native token get_balance");
        if (!ctx.consume_gas(1)) {
          co_return InvokeError{InvokeErrorCode::OutOfGas};
        }

        auto caller_address = co_await get_msg_sender(ctx);
        if (!caller_address.has_value()) {
          co_return caller_address.error();
        }

        auto balance = co_await ctx.native_token_balance(*caller_address);
        if (!balance.has_value()) {
          co_return balance.error().to_invoke_error();
        }
        if (!balance.value().has_value()) {
          co_return InvokeError{InvokeErrorCode::Deterministic};
        }
        co_return Result<amount_t, ERC20Exception>::make_ok(
            to_amount(balance.value().value()));
      });

  // mint
  builder.add<itc_erc20::ERC20, u8"mint">(mint_impl);

  // total_supply
  builder.add<itc_erc20::ERC20, u8"total_supply">(
      [](NativeSyscallContext &runtime)
          -> x23::Async<InvokeStatus<Result<amount_t, ERC20Exception>>> {
        trace("native token total_supply");
        // TODO(geoff): gas costs
        if (!runtime.consume_gas(1)) {
          co_return InvokeError{InvokeErrorCode::OutOfGas};
        }
        int128_t supply_base{};
        bool supply_base_present{};
        int128_t supply_add{};
        auto get = co_await runtime.syscall<SyscallNo::kNonnegativeIntGet>(
            kNativeTokenTotalSupplyKey, true, supply_base, supply_base_present,
            supply_add);
        if (!get.has_value()) {
          co_return NativeContractBuilder::host_fn_error_conversion(
              get.error());
        }
        int128_t total_supply;
        if (*get == SyscallErrorCode::kSyscallErrorCodeStorageKeyNotExist) {
          total_supply = 0;
        }
        else if (*get != SyscallErrorCode::kSyscallErrorCodeNone ||
                 !supply_base_present ||
                 integer_add_overflow(supply_base, supply_add) ||
                 supply_base + supply_add < 0) {
          co_return InvokeError{InvokeErrorCode::Deterministic};
        }
        else {
          total_supply = supply_base + supply_add;
        }
        co_return Result<amount_t, ERC20Exception>::make_ok(
            to_amount(total_supply));
      });

  // ERC20Permit
  builder.add<itc_erc20_permit::ERC20Permit, u8"transfer_from_signed">(
      transfer_from_signed_impl);
  builder.add<itc_erc20_permit::ERC20Permit, u8"increase_allowance_signed">(
      increase_allowance_signed_impl);
  builder.add<itc_erc20_permit::ERC20Permit, u8"delete_allowance_signed">(
      delete_allowance_signed_impl);

  // ERC165
  builder.add<itc_erc20_permit::ERC165, u8"supports_interface">(
      supports_interface_impl);
  builder.add<itc_erc20_permit::ERC165, u8"supports_interfaces">(
      supports_interfaces_impl);

  assert(builder.check_methods<itc_erc20::ERC20>());
  assert(builder.check_methods<itc_erc20_permit::ERC20Permit>());
  assert(builder.check_methods<itc_erc20_permit::ERC165>());

  return builder.build();
}

} // namespace hogd
