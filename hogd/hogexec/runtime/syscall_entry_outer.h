#pragma once

#include <array>
#include <cassert>
#include <concepts>
#include <cstddef>
#include <format>
#include <span>
#include <string>
#include <tuple>
#include <type_traits>
#include <unistd.h>
#include <utility>

#include "ffi_enums.hh"
#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h"
#include "hogd/hogexec/runtime/gas_cost_table.h"
#include "hogd/hogexec/runtime/runtime.h"
#include "hogd/hogexec/runtime/status.h"
#include "hogd/hogexec/runtime/syscall_arg_params.h"
#include "hogd/hogexec/runtime/syscall_handler.h"
#include "hogd/hogexec/runtime/syscall_meta.h"
#include "hogd/hogexec/runtime/syscall_types.h"
#include "syscall.hh"
#include "syscall_args.hh"
#include "syscall_error.hh"
#include "types.hh"
#include "x23/format.h"
#include "x23/vec.h"
#include "x23/x23.h"

namespace hogd {

// p1061r10 has structured bindings machinery implemented on
// std::index_sequence, but that seems to not implemented at the moment. auto
// [... arg_indices] = std::make_index_sequence<10>{} which p1061r10 suggests
// should work, results in arg_indices being a pack of size 0, which is
// incorrect.
// We can probably get rid of this if the std library is fixed.

template<size_t... Is> struct TupleIndexSequence {};

template<size_t... Is>
constexpr TupleIndexSequence<Is...>
from_index_sequence(std::index_sequence<Is...> /*unused*/)
{
  return TupleIndexSequence<Is...>{};
}

template<size_t Idx, typename Seq> struct access_seq;

template<size_t Idx, size_t I, size_t... Is> requires (Idx >= 1)
struct access_seq<Idx, TupleIndexSequence<I, Is...>> {
  constexpr static size_t kResult =
      access_seq<Idx - 1, TupleIndexSequence<Is...>>::kResult;
};

template<size_t I, size_t... Is>
struct access_seq<0, TupleIndexSequence<I, Is...>> {
  constexpr static size_t kResult = I;
};

template<std::size_t I, std::size_t... Is>
constexpr auto
get(TupleIndexSequence<Is...> /*unused*/)
{
  //  return std::integral_constant<size_t, Is... [I]> {};
  return std::integral_constant<
      size_t, access_seq<I, TupleIndexSequence<Is...>>::kResult>{};
}

} // namespace hogd

namespace std {

template<size_t... Is>
struct tuple_size<hogd::TupleIndexSequence<Is...>>
  : std::integral_constant<size_t, sizeof...(Is)> {};

template<size_t Idx, size_t... Is>
struct tuple_element<Idx, hogd::TupleIndexSequence<Is...>> {

  // using type = std::integral_constant<size_t, Is...[Idx]>;
  using type = std::integral_constant<
      size_t, hogd::access_seq<Idx, hogd::TupleIndexSequence<Is...>>::kResult>;
};

} // namespace std

namespace hogd {

// Creates an index sequence for a given set of args, denoting
// at what indices (into the list of uint64_t syscall inputs)
// does each logical syscall argument start.
//
// For example, a syscall that takes a span argument (2 syscall uint64_t inputs)
// and a SHA256Hash& (1 syscall uint64_t input)
// would produce the index sequence <0, 2>; the first argument takes indices 0
// and 1, and the second takes index 2.

template<size_t Base, typename Arg, typename... Args>
constexpr auto
make_offset_seqs_internal()
{
  if constexpr (sizeof...(Args) == 0) {
    return TupleIndexSequence<Base>{};
  }
  else {

    // auto [... tail] =
    //    make_offset_seqs<Base + SyscallArgParams<Arg>::kNumInputs, Args...>();
    // return TupleIndexSequence<Base, tail...>{};
    return []<size_t... Tail>(TupleIndexSequence<Tail...> /*unused*/) {
      return TupleIndexSequence<Base, Tail...>{};
    }(make_offset_seqs_internal<Base + SyscallArgParams<Arg>::kNumInputs,
                                Args...>());
  }
}

template<size_t Base, typename... Args>
constexpr auto
make_offset_seqs()
{
  if constexpr (sizeof...(Args) == 0) {
    return TupleIndexSequence<>{};
  }
  else {
    return make_offset_seqs_internal<Base, Args...>();
  }
}

template<size_t I, typename T> struct TypeNum;

template<size_t I, typename Arg, typename... Args>
requires (I <= sizeof...(Args))
struct TypeNum<I, std::tuple<Arg, Args...>> {
  using type = TypeNum<I - 1, std::tuple<Args...>>::type;
};

template<typename Arg, typename... Args>
struct TypeNum<0, std::tuple<Arg, Args...>> {
  using type = Arg;
};

template<typename Ret, typename... Args> struct SyscallTypeParams {
  using ret_t = Ret;
  using args_t = std::tuple<Args...>;

  constexpr static auto kArgOffsetIndices = make_offset_seqs<0, Args...>();
  constexpr static size_t kNumArgs = std::tuple_size<args_t>();

  template<size_t I> requires (I < sizeof...(Args))
  using arg_i = TypeNum<I, std::tuple<Args...>>::type;

  template<size_t I> constexpr static size_t offset_i()
  {
    // Can ditch access_seq, and just get the following,
    // with structured bindings for parameter packs

    // auto [... offsets] = kArgOffsetIndices;
    // return offsets...[I];

    return access_seq<
        I, std::remove_const_t<decltype(kArgOffsetIndices)>>::kResult;
  }
};

// Arguments that might be passed in as extra components to a syscall,
// that are NOT derived from the runtime memory.
class SystemCallHandler;

template<typename> struct SyscallTypes;

template<typename Ret, typename... Args>
struct SyscallTypes<Ret (*)(SystemCallHandler &, WrappedGasAmount, Args...)>
  : public SyscallTypeParams<Ret, Args...> {
  constexpr static std::tuple<SystemCallHandler &, WrappedGasAmount>
  extra_args(SystemCallHandler &handler, WrappedGasAmount total_available_gas)
  {
    return {handler, total_available_gas};
  }
};

template<typename Ret, typename... Args>
struct SyscallTypes<Ret (*)(SystemCallHandler &, Args...)>
  : public SyscallTypeParams<Ret, Args...> {
  constexpr static std::tuple<SystemCallHandler &>
  extra_args(SystemCallHandler &handler, WrappedGasAmount /*unused*/)
  {
    return {handler};
  }
};

template<typename Ret, typename... Args>
struct SyscallTypes<Ret (*)(Args...)> : public SyscallTypeParams<Ret, Args...> {
  constexpr static std::tuple<> extra_args(SystemCallHandler & /*unused*/,
                                           WrappedGasAmount /*unused*/)
  {
    return {};
  }
};

template<size_t Offset, typename ArgToParse, typename Inputs>
constexpr auto
parse(std::span<std::byte> contract_memory, Inputs const &tuple_args)
{
  using arg_params = SyscallArgParams<ArgToParse>;

  // With parameter packs in structured bindings, the lambda becomes

  // auto [... Is] =
  //    from_index_sequence(std::make_index_sequence<arg_params::kNumInputs>{});
  // auto const& [... args] = tuple_args;
  // return arg_params::parse(contract_memory, (args...[Offset + Is])...);

  return [&]<size_t... Is>(TupleIndexSequence<Is...> /*unused*/) {
    return arg_params::parse(contract_memory,
                             (std::get<Offset + Is>(tuple_args))...);
  }(from_index_sequence(std::make_index_sequence<arg_params::kNumInputs>{}));
}

// Parse inputs to mock syscall, within native_exec.
template<size_t Offset, typename ArgToParse, typename Inputs>
constexpr auto
parse_mock(Inputs const &tuple_args)
{
  using arg_params = SyscallArgParams<ArgToParse>;

  return [&]<size_t... Is>(TupleIndexSequence<Is...> /*unused*/) {
    return arg_params::parse_mock((std::get<Offset + Is>(tuple_args))...);
  }(from_index_sequence(std::make_index_sequence<arg_params::kNumInputs>{}));
}

constexpr static std::pair<HostFnStatus<SyscallErrorCode>, GasAmount>
    kFaultResult = {SyscallErrorCode::kSyscallErrorCodeFault,
                    kGenericBadSyscallCost};

auto
arg_to_xdr(auto arg)
{
  using T = std::remove_cvref_t<decltype(arg)>;

  if constexpr (std::same_as<T, int128_t>) {
    return to_xdr(arg);
  }
  else if constexpr (std::same_as<T, std::span<std::byte>> ||
                     std::same_as<T, std::span<const std::byte>>) {
    return x23::Vec<std::byte>{arg.begin(), arg.end()}
        .reinterpret<x23::Vec<uint8_t>>();
  }
  else if constexpr (std::is_pointer_v<T>) {
    using base_t = std::remove_cvref_t<decltype(arg_to_xdr(*arg))>;
    if (arg) {
      return x23::Ptr<base_t>(new base_t(arg_to_xdr(*arg)));
    }
    return x23::Ptr<base_t>(nullptr);
  }
  else {
    return arg;
  }
}

// Convert the dereferenced syscall arguments into SyscallInputType and
// SyscallOutputType. This involves an extra copy and extra allocations, and is
// not intended to be used in performance-critical settings.  Used only for
// logging syscall IO in a convenient format.
//
// The first std::tuple_size_v<SyscallInputType> arguments in dereferenced_args
// are inputs to the syscall, and the remainder are outputs.
template<typename SyscallInputType, typename... T>
auto
make_syscall_input_struct(std::tuple<T...> const &deferenced_args)
{
  return [&]<size_t... Is>(std::index_sequence<Is...> /*unused*/) {
    return SyscallInputType{arg_to_xdr(std::get<Is>(deferenced_args))...};
  }(std::make_index_sequence<std::tuple_size_v<SyscallInputType>>{});
}

template<typename SyscallInputType, typename SyscallOutputType, typename... T>
auto
make_syscall_output_struct(std::tuple<T...> const &deferenced_args)
{
  return [&]<size_t... Is>(std::index_sequence<Is...> /*unused*/) {
    return SyscallOutputType{
        arg_to_xdr(std::get<std::tuple_size_v<SyscallInputType> + Is>(
            deferenced_args))...};
  }(std::make_index_sequence<std::tuple_size_v<SyscallOutputType>>{});
}

inline std::string
syscall_error_message(HostFnStatus<SyscallErrorCode> status)
{
  if (!status.has_value()) {
    switch (status.error().code()) {
    case HostFnErrorCode::kHostExit:
      return "Host Exit (Contract Execution Panic)\n";
      break;
    case HostFnErrorCode::kHostReturn:
      return "Host Return (Contract Execution Success)\n";
      break;
    default:
      return std::format("Host Error ({})\n", x23::show(status.error().code()));
    }
  }
  assert(status.value() != SyscallErrorCode::kSyscallErrorCodeNone);
  return std::format("Syscall Error: {}", x23::show(status.value()));
}

// This logic -- validating all of the syscall inputs, calling the inner
// syscall, logging, read-only checks, and returning the result -- is common to
// both the regular and mock syscall handlers.  This is split out from
// wrap_outer so as to minimize code duplication (and reduce the chance of an
// accidental discrepancy between the test framework and the actual runtime).
template<SyscallNo Syscall, auto InnerSyscallFunc, typename SyscallInput,
         typename SyscallOutput>
constexpr std::pair<HostFnStatus<SyscallErrorCode>, GasAmount>
wrap_after_parse(SystemCallHandler &handler, WrappedGasAmount gas_remaining,
                 auto &&args)
{

  using SyscallMeta = SyscallMeta<Syscall>;
  using syscall_t = SyscallTypes<decltype(InnerSyscallFunc)>;

  if constexpr (!SyscallMeta::kIsReadOnlySyscall) {
    if (handler.is_read_only()) {
      trace("Syscall {} is not read-only, but called in read-only mode",
            Syscall);
      return {HostFnStatus<SyscallErrorCode>{
                  SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput},
              0};
    }
  }

  return [&]<size_t... Is>(std::index_sequence<Is...> /*unused*/) {
    // check whether any loads failed
    if (!((SyscallArgParams<typename syscall_t::template arg_i<Is>>::validate(
               std::get<Is>(args)) &&
           ...))) {
      if (auto *stats = handler.get_transaction_stats()) {
        stats->syscall_fault_count++;
      }
      return kFaultResult;
    }

    // Once the syscall inputs are parsed, we can print them in a formatted log
    // message.
    trace("Syscall {} Input: {}", Syscall,
          x23::show(make_syscall_input_struct<SyscallInput>(std::tuple{
              SyscallArgParams<typename syscall_t::template arg_i<Is>>::deref(
                  std::get<Is>(args))...})));

    auto extra_args = syscall_t::extra_args(handler, gas_remaining);

    // Invoke the inner syscall
    // template only required because we can't do auto [...handler] =
    // handler_pack; yet.  This would be so much cleaner in c++26.
    auto res =
        [&]<size_t... ExtraArgs>(std::index_sequence<ExtraArgs...> /*unused*/) {
          return InnerSyscallFunc(
              std::get<ExtraArgs>(extra_args)...,
              SyscallArgParams<typename syscall_t::template arg_i<Is>>::deref(
                  std::get<Is>(args))...);
        }(std::make_index_sequence<std::tuple_size_v<decltype(extra_args)>>{});

    bool is_syscall_error =
        !res.first.has_value() ||
        res.first.value() != SyscallErrorCode::kSyscallErrorCodeNone;
    trace(
        "Syscall {} Output: {}", Syscall,
        is_syscall_error
            ? syscall_error_message(res.first)
            : x23::show(make_syscall_output_struct<SyscallInput, SyscallOutput>(
                  std::tuple{
                      SyscallArgParams<typename syscall_t::template arg_i<Is>>::
                          deref(std::get<Is>(args))...})));

    return res;
  }(std::make_index_sequence<syscall_t::kNumArgs>{});
}

// Wraps a call to an inner syscall with checks to load syscall arguments
// from contract memory and check that all accesses into contract memory
// are well-formed (no OOB).
template<SyscallNo Syscall, auto InnerSyscallFunc>
constexpr std::pair<HostFnStatus<SyscallErrorCode>, GasAmount>
wrap_outer(SystemCallHandler &handler, Runtime &runtime, uint64_t arg0,
           uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4,
           uint64_t arg5, uint64_t arg6)
{
  auto contract_memory = runtime.get_memory();

  auto input_args = std::tuple{arg0, arg1, arg2, arg3, arg4, arg5, arg6};
  using syscall_t = SyscallTypes<decltype(InnerSyscallFunc)>;

  using SyscallInputType =
      std::remove_reference_t<decltype(get<std::to_underlying(Syscall) + 1>(
          SyscallInput{}))>;
  using SyscallOutputType =
      std::remove_reference_t<decltype(get<std::to_underlying(Syscall) + 1>(
          SyscallOutput{}))>;

  // Again, parameter packs as structured bindings gets rid of the annoying
  // lambda context
  return [&]<size_t... Is>(TupleIndexSequence<Is...> /*unused*/)
             -> std::pair<HostFnStatus<SyscallErrorCode>, GasAmount> {
    // Attempt to load syscall arguments from contract memory.
    auto parsed_args = std::tuple{
        parse<syscall_t::template offset_i<Is>(),
              typename syscall_t::template arg_i<Is>>(contract_memory,
                                                      input_args)...};

    return wrap_after_parse<Syscall, InnerSyscallFunc, SyscallInputType,
                            SyscallOutputType>(handler,
                                               WrappedGasAmount{
                                                   runtime.get_available_gas()},
                                               std::move(parsed_args));
  }(from_index_sequence(std::make_index_sequence<syscall_t::kNumArgs>{}));
}

} // namespace hogd
