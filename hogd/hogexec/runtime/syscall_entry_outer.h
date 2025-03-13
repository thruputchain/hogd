#pragma once

#include <cstddef>
#include <span>
#include <tuple>
#include <type_traits>
#include <utility>

#include "hogd/base/types.h"
#include "hogd/hogexec/runtime/gas_cost_table.h"
#include "hogd/hogexec/runtime/status.h"
#include "hogd/hogexec/runtime/syscall_arg_params.h"
#include "syscall.hh"
#include "types.hh"
#include "x23/utility.h"

namespace hogd {

// p1061r10 has structured bindings machinery implemented on
// std::index_sequence, but that seems to not implemented at the moment. auto
// [... arg_indices] = std::make_index_sequence<10>{} which p1061r10 suggests
// should work, results in arg_indices being a pack of size 0, which is
// incorrect.

template<size_t... Is> struct TupleIndexSequence {};

template<size_t... Is>
constexpr TupleIndexSequence<Is...>
from_index_sequence(std::index_sequence<Is...> /*unused*/)
{
  return TupleIndexSequence<Is...>{};
}

#if __cpp_pack_indexing >= 202311L

template<std::size_t I, std::size_t... Is>
constexpr auto
get(TupleIndexSequence<Is...> /*unused*/) {
  return std::integral_constant<size_t, Is... [I]> {};
}

#else

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
  return std::integral_constant<
      size_t, access_seq<I, TupleIndexSequence<Is...>>::kResult>{};
}

#endif

} // namespace hogd

namespace std {

template<size_t... Is>
struct tuple_size<hogd::TupleIndexSequence<Is...>>
  : std::integral_constant<size_t, sizeof...(Is)> {};

template<size_t Idx, size_t... Is>
struct tuple_element<Idx, hogd::TupleIndexSequence<Is...>> {

#if __cpp_pack_indexing >= 202311L
  using type = std::integral_constant<size_t, Is...[Idx]>;
#else
  using type = std::integral_constant<
      size_t, hogd::access_seq<Idx, hogd::TupleIndexSequence<Is...>>::kResult>;
#endif
};

} // namespace std

namespace hogd {

/*
template<size_t Head, typename Seq> struct append_seq;

template<size_t Head, size_t... Is>
struct append_seq<Head, tuple_index_sequence<Is...>> {
  using seq = tuple_index_sequence<Head, Is...>;
};
*/

#if __cpp_structured_bindings >= 202411L

template<size_t Base, typename Arg, typename... Args>
constexpr auto
make_offset_seqs()
{
  if constexpr (sizeof...(Args) == 0) {
    return TupleIndexSequence<Base>{};
  }
  else {
    auto [... tail] =
        make_offset_seqs<Base + SyscallArgParams<Arg>::kNumInputs, Args...>();
    return TupleIndexSequence<Base, tail...>{};
  }
}
#else
// sad fallback path
template<size_t Base, typename Arg, typename... Args>
constexpr auto
make_offset_seqs()
{
  if constexpr (sizeof...(Args) == 0) {
    return TupleIndexSequence<Base>{};
  }
  else {
    return []<size_t... Tail>(TupleIndexSequence<Tail...> /*unused*/) {
      return TupleIndexSequence<Base, Tail...>{};
    }(make_offset_seqs<Base + SyscallArgParams<Arg>::kNumInputs, Args...>());
  }
}
#endif

template<typename Ret, typename... Args> struct SyscallTypeParams {
  using ret_t = Ret;
  using args_t = std::tuple<Args...>;

  constexpr static auto kArgOffsetIndices = make_offset_seqs<0, Args...>();
  constexpr static size_t kNumArgs = std::tuple_size<args_t>();

#if __cpp_pack_indexing >= 202311L
  template<size_t I> using arg_i = Args...[I];

  template<size_t I> constexpr static size_t offset_i()
  {
    auto [... offsets] = kArgOffsetIndices;
    return offsets...[I];
  }
#else
  template<size_t I> using arg_i = x23::TypeNum<I, Args...>;
  template<size_t I> constexpr static size_t offset_i()
  {
    return access_seq<
        I, std::remove_const_t<decltype(kArgOffsetIndices)>>::kResult;
  }
#endif
};

class SystemCallHandler;

template<typename> struct SyscallTypes;

template<typename Ret, typename... Args>
struct SyscallTypes<Ret (*)(SystemCallHandler &, Args...)>
  : public SyscallTypeParams<Ret, Args...> {
  constexpr static std::tuple<SystemCallHandler &>
  handler(SystemCallHandler &handler)
  {
    return {handler};
  }
};

template<typename Ret, typename... Args>
struct SyscallTypes<Ret (*)(Args...)> : public SyscallTypeParams<Ret, Args...> {
  constexpr static std::tuple<> handler(SystemCallHandler & /*unused*/)
  {
    return {};
  }
};

#if __cpp_structured_bindings >= 202411L
template<size_t Offset, typename ArgToParse, typename Inputs>
constexpr auto
parse(std::span<std::byte> contract_memory, Inputs const &tuple_args)
{
  using arg_params = SyscallArgParams<ArgToParse>;

  auto [... Is] =
      from_index_sequence(std::make_index_sequence<arg_params::kNumInputs>{});
  auto const& [... args] = tuple_args;
  return arg_params::parse(contract_memory, (args...[Offset + Is])...);
}
#else
template<size_t Offset, typename ArgToParse, typename Inputs>
constexpr auto
parse(std::span<std::byte> contract_memory, Inputs const &tuple_args)
{
  using arg_params = SyscallArgParams<ArgToParse>;
  return [&]<size_t... Is>(TupleIndexSequence<Is...> /*unused*/) {
    return arg_params::parse(contract_memory,
                             (std::get<Offset + Is>(tuple_args))...);
  }(from_index_sequence(std::make_index_sequence<arg_params::kNumInputs>{}));
}
#endif

constexpr static std::pair<HostFnResult, GasAmount> kFaultResult = {
    {HostFnErrorCode::kNoneOrRecoverable,
     static_cast<int32_t>(SyscallErrorCode::kFault)},
    kGenericBadSyscallCost};

inline static std::pair<HostFnErrorCode, int32_t>
convert_return_t(HostFnStatus<SyscallErrorCode> const &value)
{
  if (value) {
    return {HostFnErrorCode::kNoneOrRecoverable, static_cast<int32_t>(*value)};
  }
  return {value.error().code(), 0};
}

inline static std::pair<HostFnErrorCode, int32_t>
convert_return_t(std::pair<HostFnErrorCode, int32_t> const &value)
{
  return value;
}

#if __cpp_structured_bindings >= 202411L
template<auto f>
constexpr std::pair<HostFnResult, GasAmount>
wrap_outer(SystemCallHandler &handler, std::span<std::byte> contract_memory,
           uint64_t arg0, uint64_t arg1, uint64_t arg2, uint64_t arg3,
           uint64_t arg4, uint64_t arg5)
{
  auto input_args = std::tuple{arg0, arg1, arg2, arg3, arg4, arg5};
  using syscall_t = SyscallTypes<decltype(f)>;
  auto [... Is] =
      from_index_sequence(std::make_index_sequence<syscall_t::kNumArgs>{});

  auto [... parsed_args] =
      std::tuple{parse<syscall_t::template offset_i<Is>(),
                       typename syscall_t::template arg_i<Is>>(contract_memory,
                                                               input_args)...};

  if (!((SyscallArgParams<typename syscall_t::template arg_i<Is>>::validate(
             parsed_args...[Is]) &&
         ...))) {
    return kFaultResult;
  }

  auto [... handler_pack] = syscall_t::handler(handler);

  auto res = f(handler_pack...,
               SyscallArgParams<typename syscall_t::template arg_i<Is>>::deref(
                   parsed_args...[Is])...);

  return {convert_return_t(res.first), res.second};
}
#else

template<auto f>
constexpr std::pair<HostFnResult, GasAmount>
wrap_outer(SystemCallHandler &handler, std::span<std::byte> contract_memory,
           uint64_t arg0, uint64_t arg1, uint64_t arg2, uint64_t arg3,
           uint64_t arg4, uint64_t arg5)
{
  auto input_args = std::tuple{arg0, arg1, arg2, arg3, arg4, arg5};
  using syscall_t = SyscallTypes<decltype(f)>;

  return [&]<size_t... Is>(TupleIndexSequence<Is...> /*unused*/)
             -> std::pair<HostFnResult, GasAmount> {
    auto parsed_args = std::tuple{
        parse<syscall_t::template offset_i<Is>(),
              typename syscall_t::template arg_i<Is>>(contract_memory,
                                                      input_args)...};

    if (!((SyscallArgParams<typename syscall_t::template arg_i<Is>>::validate(
               std::get<Is>(parsed_args)) &&
           ...))) {
      return kFaultResult;
    }

    auto handler_pack = syscall_t::handler(handler);

    if constexpr (std::tuple_size_v<decltype(handler_pack)> == 1) {
      auto res =
          f(std::get<0>(handler_pack),
            SyscallArgParams<typename syscall_t::template arg_i<Is>>::deref(
                std::get<Is>(parsed_args))...);

      return {convert_return_t(res.first), res.second};
    }
    else {
      auto res =
          f(SyscallArgParams<typename syscall_t::template arg_i<Is>>::deref(
              std::get<Is>(parsed_args))...);

      return {convert_return_t(res.first), res.second};
    }
  }(from_index_sequence(std::make_index_sequence<syscall_t::kNumArgs>{}));
}

#endif

} // namespace hogd
