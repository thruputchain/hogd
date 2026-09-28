#include "hogd/hog_exec/precompiles/precompile_native_token.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <tuple>
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
#include "hogd/hog_exec/runtime/native_runtime.h"
#include "hogd/hog_exec/runtime/native_syscall_context.h"
#include "hogd/hog_exec/runtime/serialize_calldata.h"
#include "standard_asset_events.hh"
#include "syscall.hh"
#include "syscall_error.hh"
#include "types.hh"
#include "x23/async.h"
#include "x23/format.h"
#include "x23/serialize_cxs.h"
#include "x23/vec.h"

namespace hogd {

using ERC20Exception =
    ContractException<itc_erc20::ERC20Error, 0xb19947acb2fa14ffULL>;

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
  co_return *r2 == SyscallErrorCode::kSyscallErrorCodeNone;
}

// For now, the typedefs exported by a contract aren't added to the generated
// spec, unfortunately
using amount_t = std::tuple<uint64_t, uint64_t>;
using difference_t = std::tuple<int64_t, uint64_t>;

constexpr uint128_t kMaxInt128 = std::numeric_limits<int128_t>::max();

using MintBurnStatus = InvokeStatus<Result<void, ERC20Exception>>;

// Mirror the reference's add_to_total_supply (the SDK's plain, non-strict
// non_negative_int_inc): combine the increment with the in-transaction values
// only -- no snapshot force-load, and no invoke-time range check against the
// committed base, which is validated at commit instead. The one invoke-time
// failure is an overflow of the accumulated in-tx add, which the SDK reports
// by throwing LibraryExn::NonNegativeIntOverflow -- returned here as the
// byte-identical library abort. Returns nullopt once the increment is staged.
x23::Async<std::optional<MintBurnStatus>>
add_to_total_supply(NativeSyscallContext &ctx, int128_t diff)
{
  int128_t set_value{};
  bool set_value_present{};
  int128_t add_value{};
  auto get = co_await ctx.syscall<SyscallNo::kNonnegativeIntGet>(
      kNativeTokenTotalSupplyKey,
      /*load_from_snapshot=*/false, set_value, set_value_present, add_value);
  if (!get) {
    co_return MintBurnStatus{
        NativeContractBuilder::host_fn_error_conversion(get.error())};
  }
  if (*get != SyscallErrorCode::kSyscallErrorCodeNone) {
    co_return MintBurnStatus{InvokeError{InvokeErrorCode::Deterministic}};
  }
  if (integer_add_overflow(add_value, diff)) {
    co_return MintBurnStatus{
        {Result<void, ERC20Exception>::make_err(LibraryException{
            LibraryExn(LibraryExnKind::kLibraryExnNonNegativeIntOverflow)})}};
  }
  const int128_t *set_ptr = set_value_present ? &set_value : nullptr;
  auto set = co_await ctx.syscall<SyscallNo::kNonnegativeIntSetAdd>(
      kNativeTokenTotalSupplyKey, set_ptr, add_value + diff);
  if (!set) {
    co_return MintBurnStatus{
        NativeContractBuilder::host_fn_error_conversion(set.error())};
  }
  if (*set != SyscallErrorCode::kSyscallErrorCodeNone) {
    co_return MintBurnStatus{InvokeError{InvokeErrorCode::Deterministic}};
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

  const uint128_t unsigned_amount =
      uint128_t{std::get<0>(amount)} << 64 | std::get<1>(amount);
  if (unsigned_amount > kMaxInt128) {
    co_return co_await erc20_error(itc_erc20::ERC20Error::TooLarge);
  }
  const int128_t signed_amount = unsigned_amount;

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

x23::Async<InvokeStatus<Result<void, ERC20Exception>>>
burn_impl(NativeSyscallContext &ctx, ContractAddress owner, amount_t amount)
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

  const uint128_t unsigned_amount =
      uint128_t{std::get<0>(amount)} << 64 | std::get<1>(amount);
  if (unsigned_amount > kMaxInt128) {
    co_return co_await erc20_error(itc_erc20::ERC20Error::TooLarge);
  }
  const int128_t signed_amount = unsigned_amount;

  // Match the reference ERC20's strict balance debit: reject before staging
  // any state if the requested burn exceeds the owner's running balance.
  auto owner_balance = co_await ctx.native_token_balance(owner);
  if (!owner_balance) {
    co_return owner_balance.error().to_invoke_error();
  }
  if (!owner_balance.value().has_value()) {
    co_return InvokeError{InvokeErrorCode::Deterministic};
  }
  if (signed_amount > owner_balance.value().value()) {
    co_return co_await erc20_error(itc_erc20::ERC20Error::TooLarge);
  }

  auto debit = co_await ctx.modify_native_token_balance(owner, -signed_amount);
  if (!debit) {
    co_return debit.error().to_invoke_error();
  }
  if (!debit.value().has_value()) {
    co_return InvokeError{InvokeErrorCode::Deterministic};
  }

  // Reference order: the balance debit first, then the deferred supply
  // decrement; a decrement past the committed supply is caught at commit.
  if (auto supply_failure = co_await add_to_total_supply(ctx, -signed_amount)) {
    co_return std::move(*supply_failure);
  }

  const NativeTokenBurnEvent event{.from_address = owner,
                                   .amount = to_xdr(signed_amount)};
  auto event_buf =
      make_event_from_cxs_struct(native_token_burn_event_signature(), event);
  if (!event_buf) {
    co_return InvokeError{InvokeErrorCode::Deterministic};
  }
  if (auto emitted = co_await emit_event<2>(ctx, *event_buf, "burn");
      !emitted) {
    co_return emitted.error();
  }
  co_return Result<void, ERC20Exception>::make_ok();
}

// The owner-to-receiver leg every spending path shares -- transfer,
// transfer_from and transfer_from_signed -- mirroring the reference's
// add_to_balance(owner, -diff), add_to_balance(receiver, diff), then the
// Transfer event. `amount` is already range-checked to [0, INT128_MAX], and
// any authorization (caller, allowance, grant) has already been checked.
// `entry_point` names the caller in log lines.
//
// Generic over the caller's result type, since the ERC20Permit methods reject
// with the same ERC20Error through their own interface's Result:
// `Erc20ExceptionT` is the ERC20Error exception within `ResultT`.
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

  // No credit-side overflow check: total supply (~2^59) keeps any reachable
  // receiver balance + amount far below INT128_MAX, so the reference's
  // checked add on the credit leg cannot fire. HogDb's commit-time
  // add-capacity bound remains the backstop for hand-crafted states.

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

  // For now, amount_t is a uint, so we do not need to check for negative
  // amounts here. adding the assertion in case that changes
  uint128_t amount_unsigned_int =
      uint128_t{std::get<0>(amount)} << 64 | std::get<1>(amount);

  auto owner = co_await get_msg_sender(ctx);
  if (!owner.has_value()) {
    co_return owner.error();
  }

  // An amount above INT128_MAX cannot be represented as a signed balance delta;
  // reject with TooLarge, matching the itc_erc20 reference's observable result.
  if (amount_unsigned_int > kMaxInt128) {
    co_return co_await erc20_error(itc_erc20::ERC20Error::TooLarge);
  }
  int128_t amount_signed_int = amount_unsigned_int;

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

  uint128_t amount_unsigned_int =
      uint128_t{std::get<0>(amount)} << 64 | std::get<1>(amount);

  // get spender
  auto spender = co_await get_msg_sender(ctx);
  if (!spender.has_value()) {
    co_return spender.error();
  }

  // An amount above INT128_MAX cannot be represented as a signed balance delta;
  // reject with TooLarge, matching the itc_erc20 reference's observable result.
  if (amount_unsigned_int > kMaxInt128) {
    co_return co_await erc20_error(itc_erc20::ERC20Error::TooLarge);
  }
  int128_t amount_signed_int = amount_unsigned_int;

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

// Emit the Approval event for an allowance of `owner`'s changed by `diff` for
// `spender`, as the reference's log_approval_event does.
x23::Async<InvokeStatus<void>>
emit_approval_event(NativeSyscallContext &ctx, ContractAddress const &owner,
                    ContractAddress const &spender, int128_t diff,
                    std::string_view entry_point)
{
  auto event_buf =
      make_event_from_cxs_struct(native_token_approval_event_signature(),
                                 ApprovalEvent{
                                     .owner_address = owner,
                                     .spender_address = spender,
                                     .amount = to_xdr(diff),
                                 });
  if (!event_buf)
    co_return InvokeError{InvokeErrorCode::Deterministic};

  co_return co_await emit_event<3>(ctx, *event_buf, entry_point);
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

  // Granting oneself an allowance is meaningless; itc_erc20 rejects it at
  // get_allowance_key with ERC20Error::Internal -- mirror that.
  if (*owner == spender) {
    co_return co_await erc20_error(itc_erc20::ERC20Error::Internal);
  }

  int128_t amount_signed_int =
      int128_t{std::get<0>(amount)} << 64 | std::get<1>(amount);

  auto r = co_await adjust_allowance(ctx, *owner, spender, amount_signed_int);
  if (!r.has_value()) {
    co_return r.error();
  }
  if (!r.value()) {
    co_return co_await erc20_error(itc_erc20::ERC20Error::TooLarge);
  }

  if (auto err =
          co_await emit_approval_event(ctx, *owner, spender, amount_signed_int,
                                       "adjust_allowance");
      !err.has_value()) {
    co_return err.error();
  }

  co_return Result<void, ERC20Exception>::make_ok();
}

x23::Async<InvokeStatus<Result<amount_t, ERC20Exception>>>
get_allowance_impl(NativeSyscallContext &ctx, ContractAddress owner,
                   ContractAddress spender)
{
  trace("native token get_allowance: owner {} spender {}", x23::show(owner),
        x23::show(spender));
  if (!ctx.consume_gas(1)) {
    co_return InvokeError{InvokeErrorCode::OutOfGas};
  }

  auto allow_key_r = co_await allowance_key(ctx, owner, spender);
  if (!allow_key_r.has_value()) {
    co_return allow_key_r.error();
  }
  auto const &allow_key = *allow_key_r;

  int128_t output_set{};
  bool output_set_opt{};
  int128_t output_add{};
  auto r = co_await ctx.syscall<SyscallNo::kNonnegativeIntGet>(allow_key, true,
                                                               output_set,
                                                               output_set_opt,
                                                               output_add);
  if (!r.has_value()) {
    co_return NativeContractBuilder::host_fn_error_conversion(r.error());
  }
  if (*r == SyscallErrorCode::kSyscallErrorCodeStorageKeyNotExist) {
    co_return Result<amount_t, ERC20Exception>::make_ok(
        std::make_tuple(uint64_t{0}, uint64_t{0}));
  }
  if (*r != SyscallErrorCode::kSyscallErrorCodeNone) {
    co_return InvokeError{InvokeErrorCode::Deterministic};
  }

  CHECK(output_set_opt);
  auto [total, of] = overflowing_add(output_set, output_add);
  if (of) {
    co_return InvokeError{InvokeErrorCode::Deterministic};
  }
  uint128_t utotal = std::max<int128_t>(total, 0);
  co_return Result<amount_t, ERC20Exception>::make_ok(
      std::make_tuple(utotal >> 64, utotal & 0xffffffffffffffffULL));
}

// ERC20Permit (groundhog-erc20-permit-if): approvals that arrive as auth
// tokens rather than as a transaction from the owner. The RV32 reference is
// itc_erc20_permit, which is itc_erc20 with this interface on top.

using PermitErc20Exception =
    ContractException<itc_erc20_permit::ERC20Error, 0xb19947acb2fa14ffULL>;
using PermitException = ContractException<itc_erc20_permit::ERC20PermitError,
                                          0xe8abb072f5171148ULL>;
using PermitResult = Result<void, PermitErc20Exception, PermitException>;

static_assert(ERC20Exception::kSelector == PermitErc20Exception::kSelector,
              "ERC20Error must reject identically through either interface");

// A consumed PermitGrant and the address that minted it -- the owner whose
// tokens it authorizes spending.
struct ConsumedGrant {
  itc_erc20_permit::PermitGrant grant;
  ContractAddress owner;
};

using ConsumeGrantResult = std::variant<ConsumedGrant, LibraryException>;

// Consume `token` as the SDK's `Signed<PermitGrant>::consume` does, returning
// the grant or the library exception the SDK would throw:
//
//  - the ConsumeAuthToken syscall rejecting the handle (not this contract's
//    to consume, expired, already spent) throws LibraryExn::Syscall;
//  - a payload that is not exactly one CXS PermitGrant throws
//    AuthTokenError::ConsumeDecodeFailure;
//  - child tokens, which a PermitGrant has no Signed field to receive, throw
//    AuthTokenError::ReceiveAuthHandlesFailure. They are unwrapped into this
//    frame by the syscall either way, as they are for the reference.
x23::Async<InvokeStatus<ConsumeGrantResult>>
consume_permit_grant(NativeSyscallContext &ctx, SignedToken const &token)
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
    co_return ConsumeGrantResult{
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

  itc_erc20_permit::PermitGrant grant{};
  auto const *payload_bytes = reinterpret_cast<uint8_t const *>(payload.data());
  if (!x23::deserialize_cxs(&grant, std::span<uint8_t const>(payload_bytes,
                                                             payload_len))) {
    co_return ConsumeGrantResult{auth_token_exception(
        AuthTokenErrorKind::kAuthTokenConsumeDecodeFailure)};
  }
  if (child_count != 0) {
    co_return ConsumeGrantResult{auth_token_exception(
        AuthTokenErrorKind::kAuthTokenReceiveAuthHandlesFailure)};
  }
  co_return ConsumeGrantResult{ConsumedGrant{.grant = grant, .owner = minter}};
}

// The grant's amount as a signed delta, or nullopt when it exceeds INT128_MAX
// -- the reference's amount_to_diff, which throws ERC20Error::TooLarge.
std::optional<int128_t>
grant_amount(itc_erc20_permit::PermitGrant const &grant)
{
  const uint128_t amount = from_xdr(grant.amount);
  if (amount > kMaxInt128) {
    return std::nullopt;
  }
  return static_cast<int128_t>(amount);
}

x23::Async<InvokeStatus<PermitResult>>
permit_impl(NativeSyscallContext &ctx, SignedToken grant_token)
{
  trace("native token permit: handle {}", grant_token.handle);
  if (!ctx.consume_gas(1)) {
    co_return InvokeError{InvokeErrorCode::OutOfGas};
  }

  auto consumed = co_await consume_permit_grant(ctx, grant_token);
  if (!consumed.has_value()) {
    co_return consumed.error();
  }
  if (auto *exn = std::get_if<LibraryException>(&*consumed)) {
    co_return PermitResult::make_err(std::move(*exn));
  }
  auto const &[grant, owner] = std::get<ConsumedGrant>(*consumed);

  auto diff = grant_amount(grant);
  if (!diff) {
    co_return PermitResult::make_err(
        PermitErc20Exception{itc_erc20_permit::ERC20Error::TooLarge});
  }
  // An owner granting itself an allowance: the reference's get_allowance_key
  // rejects it with Internal, as adjust_allowance_impl does.
  if (owner == grant.spender) {
    co_return PermitResult::make_err(
        PermitErc20Exception{itc_erc20_permit::ERC20Error::Internal});
  }

  // The allowance grows by the grant rather than being set to it, as
  // adjust_allowance does: a grant is single-use, so there is no second grant
  // for a set-to-value to defend against.
  auto adjusted = co_await adjust_allowance(ctx, owner, grant.spender, *diff);
  if (!adjusted.has_value()) {
    co_return adjusted.error();
  }
  if (!adjusted.value()) {
    co_return PermitResult::make_err(
        PermitErc20Exception{itc_erc20_permit::ERC20Error::TooLarge});
  }

  if (auto err = co_await emit_approval_event(ctx, owner, grant.spender, *diff,
                                              "permit");
      !err.has_value()) {
    co_return err.error();
  }
  co_return PermitResult::make_ok();
}

x23::Async<InvokeStatus<PermitResult>>
transfer_from_signed_impl(NativeSyscallContext &ctx, SignedToken grant_token,
                          ContractAddress receiver)
{
  trace("native token transfer_from_signed: handle {} receiver {}",
        grant_token.handle, x23::show(receiver));
  if (!ctx.consume_gas(1)) {
    co_return InvokeError{InvokeErrorCode::OutOfGas};
  }

  auto consumed = co_await consume_permit_grant(ctx, grant_token);
  if (!consumed.has_value()) {
    co_return consumed.error();
  }
  if (auto *exn = std::get_if<LibraryException>(&*consumed)) {
    co_return PermitResult::make_err(std::move(*exn));
  }
  auto const &[grant, owner] = std::get<ConsumedGrant>(*consumed);

  // The grant names who may spend it; no allowance is written or read, so the
  // caller being that spender is the whole authorization check.
  auto caller = co_await get_msg_sender(ctx);
  if (!caller.has_value()) {
    co_return caller.error();
  }
  if (*caller != grant.spender) {
    co_return PermitResult::make_err(
        PermitException{itc_erc20_permit::ERC20PermitError::NotSpender});
  }

  auto amount = grant_amount(grant);
  if (!amount) {
    co_return PermitResult::make_err(
        PermitErc20Exception{itc_erc20_permit::ERC20Error::TooLarge});
  }

  co_return co_await move_balance<PermitResult, PermitErc20Exception>(
      ctx, owner, receiver, *amount, "transfer_from_signed");
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
      compute_event_signature(u8"0.1::groundhog-erc20-if::TransferEvent");
  return kSignature;
}

Topic const &
native_token_approval_event_signature()
{
  static const Topic kSignature =
      compute_event_signature(u8"0.1::groundhog-erc20-if::ApprovalEvent");
  return kSignature;
}

Topic const &
native_token_mint_event_signature()
{
  static const Topic kSignature =
      compute_event_signature(u8"0.1::groundhog-erc20-if::MintEvent");
  return kSignature;
}

Topic const &
native_token_burn_event_signature()
{
  static const Topic kSignature =
      compute_event_signature(u8"0.1::groundhog-erc20-if::BurnEvent");
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
        assert(balance.value().value() >= 0);
        uint128_t ubalance = balance.value().value();
        co_return Result<amount_t, ERC20Exception>::make_ok(
            std::make_tuple(ubalance >> 64, ubalance & 0xffffffffffffffffULL));
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
        assert(balance.value().value() >= 0);
        uint128_t ubalance = balance.value().value();
        co_return Result<amount_t, ERC20Exception>::make_ok(
            std::make_tuple(ubalance >> 64, ubalance & 0xffffffffffffffffULL));
      });

  // mint
  builder.add<itc_erc20::ERC20, u8"mint">(mint_impl);

  // burn
  builder.add<itc_erc20::ERC20, u8"burn">(burn_impl);

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
        uint128_t total_supply;
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
            std::make_tuple(total_supply >> 64,
                            total_supply & 0xffffffffffffffffULL));
      });

  // ERC20Permit
  builder.add<itc_erc20_permit::ERC20Permit, u8"permit">(permit_impl);
  builder.add<itc_erc20_permit::ERC20Permit, u8"transfer_from_signed">(
      transfer_from_signed_impl);

  assert(builder.check_methods<itc_erc20::ERC20>());
  assert(builder.check_methods<itc_erc20_permit::ERC20Permit>());

  return builder.build();
}

} // namespace hogd
