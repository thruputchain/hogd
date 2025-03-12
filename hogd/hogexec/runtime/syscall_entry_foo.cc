#include "hogd/base/types.h"
#include "hogd/hogexec/runtime/gas_cost_table.h"
#include "hogd/hogexec/runtime/status.h"
#include "hogd/hogexec/runtime/syscall_entry.h"
#include "hogd/hogexec/runtime/syscall_inner.h"
#include "syscall.hh"
#include "types.hh"
#include <concepts>
#include <cstddef>
#include <optional>
#include <span>
#include <tuple>
#include <utility>

namespace hogd {

template<typename T> struct arg_params;

template<> struct arg_params<uint64_t> {
  constexpr static size_t kNumInputs = 1;
  constexpr static uint64_t parse(uint64_t input) { return input; }

  constexpr static bool validate(uint64_t /*unused*/) { return true;}

  constexpr static uint64_t deref(uint64_t value) { return value; }
};

template<> struct arg_params<std::span<std::byte>> {
  constexpr static size_t kNumInputs = 2;
  constexpr static std::optional<std::span<std::byte>> parse(uint64_t arg0, uint64_t arg1)
  {
    return std::nullopt;
  }
  constexpr static bool validate(std::optional<std::span<std::byte>> arg) { return arg.has_value();}
};

template<size_t Head, typename Seq> struct append_seq;

template<size_t Head, size_t... Is>
struct append_seq<Head, std::index_sequence<Is...>> {
  using seq = std::index_sequence<Head, Is...>;
};

template<size_t base, typename... Args> struct make_arg_offset_seqs;

template<size_t base, typename Arg, typename... Args>
struct make_arg_offset_seqs<base, Arg, Args...> {
  using cur_arg_params = arg_params<Arg>;
  using tail_t =
      make_arg_offset_seqs<base + cur_arg_params::kNumInputs, Args...>;
  using indices = append_seq<base, typename tail_t::indices>::seq;
};

template<size_t base> struct make_arg_offset_seqs<base> {
  using indices = std::index_sequence<>;
};

template<size_t idx, typename seq>
struct access_seq;

template<size_t idx, size_t I, size_t... Is> requires (idx >= 1)
struct access_seq<idx, std::index_sequence<I, Is...>> {
    constexpr static size_t kResult = access_seq<idx-1, std::index_sequence<Is...>>::kResult;
};

template<size_t I, size_t... Is>
struct access_seq<0, std::index_sequence<I, Is...>> {
    constexpr static size_t kResult = I;
};

// tests
static_assert(std::same_as<make_arg_offset_seqs<0, uint64_t, uint64_t, uint64_t>::indices, std::index_sequence<0, 1, 2>>);

template<typename> struct syscall_types;

template<typename Ret, typename... Args>
struct syscall_types<Ret (*)(Args...)> {
  using ret_t = Ret;
  using args_t = std::tuple<Args...>;
  using arg_offset_indices = typename make_arg_offset_seqs<0, Args...>::indices;
  constexpr static size_t kNumArgs = std::tuple_size<args_t>();

  template<size_t I>
  using arg_i = Args...[I];

  template<size_t I>
  constexpr static size_t offset_i = access_seq<I, arg_offset_indices>::kResult;
};

constexpr int test_fn(uint64_t first, uint64_t second) {
    return first * second;
}

template<size_t arg_offset, typename ArgToParse, typename input_args>
constexpr auto
parse(input_args const &tuple_args)
{
  using arg_stats = arg_params<ArgToParse>;
  return [&]<size_t... Is>(std::index_sequence<Is...>) {
    auto [...args] = tuple_args;

    return arg_stats::parse(args...[arg_offset + Is]...);
    }(std::make_index_sequence<arg_stats::kNumInputs>{});
}


constexpr static std::pair<HostFnResult, GasAmount> kFaultResult =
    {{HostFnErrorCode::kNoneOrRecoverable, 
             static_cast<int32_t>(SyscallErrorCode::kFault)}, kGenericBadSyscallCost};

template<auto f>
constexpr auto
wrap_outer(uint64_t arg0, uint64_t arg1, uint64_t arg2, uint64_t arg3)
{
  auto input_args = std::tuple{arg0, arg1, arg2, arg3};
  using syscall_t = syscall_types<decltype(f)>;
  return [&input_args]<size_t... arg_indices>(std::index_sequence<arg_indices...>) {
    
    auto [...parsed_args] = std::tuple{
        parse<syscall_t::template offset_i<arg_indices>,
                typename syscall_t::template arg_i<arg_indices>>(input_args)...
    };

    auto validate_res = (arg_params<typename syscall_t::template arg_i<arg_indices>>::validate(parsed_args...[arg_indices]) && ...);

    if (!validate_res) {
        return 0;
        //return kFaultResult;
    }

    auto res = f(arg_params<typename syscall_t::template arg_i<arg_indices>>::deref(parsed_args...[arg_indices])...);

    return res;




  }(std::make_index_sequence<syscall_t::kNumArgs>{});
}


constexpr static auto x = wrap_outer<test_fn>(2, 5, 3, 4);
static_assert(x == 10);
/*
void test_foo(std::span<std::byte> arg) {}

constexpr static auto y = wrap_outer<test_foo>(1, 2, 3, 4);
static_assert(y == std::tuple<uint64_t, uint64_t> {0, 0});
*/
} // namespace hogd
