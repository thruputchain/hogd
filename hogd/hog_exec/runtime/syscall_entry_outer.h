#pragma once

#include <algorithm>
#include <array>
#include <cassert>
#include <concepts>
#include <cstddef>
#include <cstdio>
#include <exception>
#include <format>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>

#include "ffi_enums.hh"
#include "hogd/base/compiler.h"
#include "hogd/base/log.h"
#include "hogd/base/string.h"
#include "hogd/base/types.h"
#include "hogd/hog_exec/common/status.h"
#include "hogd/hog_exec/components/alias_guard.h"
#include "hogd/hog_exec/components/ffi_helpers.h"
#include "hogd/hog_exec/runtime/gas_cost_table.h"
#include "hogd/hog_exec/runtime/runtime.h"
#include "hogd/hog_exec/runtime/runtime_exports.h"
#include "hogd/hog_exec/runtime/syscall_arg_params.h"
#include "hogd/hog_exec/runtime/syscall_handler.h"
#include "hogd/hog_exec/runtime/syscall_meta_concept.h"
#include "hogd/hog_exec/runtime/syscall_types.h"
#include "syscall.hh"
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

// Helper to check if a syscall error code is in the allowed list for a syscall.
// Used in debug builds to validate that syscalls only return documented errors.
template<typename SyscallMeta>
constexpr bool
is_allowed_error_code(SyscallErrorCode code)
{
  // kSyscallErrorCodeNone is always allowed (success case)
  // kSyscallErrorCodeFault is allowed as all syscalls can be given an invalid
  // input
  if (code == SyscallErrorCode::kSyscallErrorCodeNone ||
      code == SyscallErrorCode::kSyscallErrorCodeFault) {
    return true;
  }

  // Mutable syscalls always allow kSyscallErrorCodeInvalidSyscallInput:
  // the read-only wrapper returns it when a mutable syscall is called in a
  // read-only frame, regardless of what the inner syscall documents.
  if constexpr (!SyscallMeta::kIsReadOnlySyscall) {
    if (code == SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput) {
      return true;
    }
  }

  return std::ranges::any_of(SyscallMeta::kAllowedErrors,
                             [code](auto allowed) { return code == allowed; });
}

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

template<typename Ret, typename... Args>
requires (is_syscall_arg_param<Args> && ...)
struct SyscallTypeParams {
  using ret_t = Ret;
  using args_t = std::tuple<Args...>;

  constexpr static auto kArgOffsetIndices = make_offset_seqs<0, Args...>();
  constexpr static size_t kNumArgs = std::tuple_size<args_t>();

  // Total number of register argument words this syscall consumes (each arg
  // type contributes SyscallArgParams<Arg>::kNumInputs: scalars 1, spans 2,
  // etc.). syscall_entry passes arg0=syscall number plus arg1..arg7, so this
  // must not exceed 7; see the static_assert in wrap_outer.
  constexpr static size_t kTotalInputs =
      (size_t{0} + ... + SyscallArgParams<Args>::kNumInputs);

  template<size_t I> requires (I < sizeof...(Args))
  using arg_i = TypeNum<I, std::tuple<Args...>>::type;

  template<size_t I> requires (I < sizeof...(Args))
  constexpr static size_t offset_i()
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
requires (is_syscall_arg_param<Args> && ...)
struct SyscallTypes<Ret (*)(SystemCallHandler &, WrappedGasAmount, Args...)>
  : public SyscallTypeParams<Ret, Args...> {
  constexpr static std::tuple<SystemCallHandler &, WrappedGasAmount>
  extra_args(SystemCallHandler &handler, WrappedGasAmount total_available_gas)
  {
    return {handler, total_available_gas};
  }
};

template<typename Ret, typename... Args>
requires (is_syscall_arg_param<Args> && ...)
struct SyscallTypes<Ret (*)(SystemCallHandler &, Args...)>
  : public SyscallTypeParams<Ret, Args...> {
  constexpr static std::tuple<SystemCallHandler &>
  extra_args(SystemCallHandler &handler, WrappedGasAmount /*unused*/)
  {
    return {handler};
  }
};

template<typename Ret, typename... Args>
requires (is_syscall_arg_param<Args> && ...)
struct SyscallTypes<Ret (*)(Args...)> : public SyscallTypeParams<Ret, Args...> {
  constexpr static std::tuple<> extra_args(SystemCallHandler & /*unused*/,
                                           WrappedGasAmount /*unused*/)
  {
    return {};
  }
};

template<size_t Offset, typename ArgToParse, typename Inputs>
requires (is_syscall_arg_param<ArgToParse>)
constexpr auto
parse(AliasGuard &guard, std::span<std::byte> contract_memory,
      Inputs const &tuple_args)
{
  using arg_params = SyscallArgParams<ArgToParse>;

  // With parameter packs in structured bindings, the lambda becomes

  // auto [... Is] =
  //    from_index_sequence(std::make_index_sequence<arg_params::kNumInputs>{});
  // auto const& [... args] = tuple_args;
  // return arg_params::parse(contract_memory, (args...[Offset + Is])...);

  return [&]<size_t... Is>(TupleIndexSequence<Is...> /*unused*/) {
    return arg_params::parse(guard, contract_memory,
                             (std::get<Offset + Is>(tuple_args))...);
  }(from_index_sequence(std::make_index_sequence<arg_params::kNumInputs>{}));
}

template<typename T, uint32_t... Args>
concept is_parseable =
    requires(AliasGuard &guard, std::span<std::byte> contract_memory) {
      SyscallArgParams<T>::parse(guard, contract_memory, Args...);
    };
// We want just one path for parsing int128; this simplifies the template
// deduction, making it easier to reason about what template is invoked where.
static_assert(is_parseable<const int128_t &, 0>);
static_assert(!is_parseable<int128_t, 0>);

// Parse inputs to mock syscall, within native_exec.
template<size_t Offset, typename ArgToParse, typename Inputs>
requires (is_syscall_arg_param<ArgToParse>)
constexpr auto
parse_mock(AliasGuard &guard, Inputs const &tuple_args)
{
  using arg_params = SyscallArgParams<ArgToParse>;

  return [&]<size_t... Is>(TupleIndexSequence<Is...> /*unused*/) {
    return arg_params::parse_mock(guard,
                                  (std::get<Offset + Is>(tuple_args))...);
  }(from_index_sequence(std::make_index_sequence<arg_params::kNumInputs>{}));
}

constexpr static std::pair<HostFnStatus<SyscallErrorCode>, WrappedGasAmount>
    kFaultResult = {SyscallErrorCode::kSyscallErrorCodeFault,
                    WrappedGasAmount{kGenericBadSyscallCost}};

template<typename T>
concept is_std_optional = requires {
  typename T::value_type;
} && std::same_as<T, std::optional<typename T::value_type>>;

template<typename T>
std::string
show_type(T const &arg)
{
  if constexpr (x23::is_xdr_type<T>) {
    return x23::show(arg);
  }
  else if constexpr (std::is_integral_v<T>) {
    return std::format("{}", arg);
  }
  else if constexpr (std::same_as<T, std::span<std::byte>> ||
                     std::same_as<T, std::span<const std::byte>>) {
    return "[" + encode_hex(arg) + "]";
  }
  else if constexpr (std::is_pointer_v<T>) {
    if (arg) {
      return "[" + show_type<std::remove_cvref_t<decltype(*arg)>>(*arg) + "]";
    }
    return "[null]";
  }
  else if constexpr (is_std_optional<T>) {
    static_assert(!std::same_as<T, ContractAddress>);
    if (arg) {
      return "[" + show_type<typename T::value_type>(*arg) + "]";
    }
    return "[null]";
  }
  else if constexpr (is_ffi_helper_required<T>) {
    auto cpp_version = FFIHelperType<T>::to_cpp(arg);
    return "[" + show_type(cpp_version) + "]";
  }
  else {
    static_assert(false, "unknown type");
  }
}

template<typename T>
std::string
make_msg(std::string_view name, T const &arg)
{
  return std::format("  {}: {}", name, show_type<T>(arg));
}

template<typename SyscallMeta, typename... T>
auto
make_syscall_input_message(std::tuple<T...> const &deferenced_args)
    requires (sizeof...(T) >= SyscallMeta::kInputNames.size())
{
  auto input_msgs = [&]<size_t... Is>(std::index_sequence<Is...> /*unused*/) {
    return std::tuple{make_msg<
        typename std::decay_t<decltype(std::get<Is>(deferenced_args))>>(
        std::get<Is>(SyscallMeta::kInputNames),
        std::get<Is>(deferenced_args))...};
  }(std::make_index_sequence<SyscallMeta::kInputNames.size()>{});

  std::string out;
  [&]<size_t... Is>(std::index_sequence<Is...> /*unused*/) {
    ((out += std::get<Is>(input_msgs) + "\n"), ...);
  }(std::make_index_sequence<SyscallMeta::kInputNames.size()>{});

  return "{\n" + out + "}";
}

template<typename SyscallMeta, typename... T>
auto
make_syscall_output_message(std::tuple<T...> const &deferenced_args)
    requires (sizeof...(T) == SyscallMeta::kOutputNames.size() +
                                  SyscallMeta::kInputNames.size())
{
  auto output_msgs = [&]<size_t... Is>(std::index_sequence<Is...> /*unused*/) {
    return std::tuple{make_msg<typename std::decay_t<
        decltype(std::get<SyscallMeta::kInputNames.size() + Is>(
            deferenced_args))>>(std::get<Is>(SyscallMeta::kOutputNames),
                                std::get<SyscallMeta::kInputNames.size() + Is>(
                                    deferenced_args))...};
  }(std::make_index_sequence<SyscallMeta::kOutputNames.size()>{});

  std::string out;
  [&]<size_t... Is>(std::index_sequence<Is...> /*unused*/) {
    ((out += std::get<Is>(output_msgs) + "\n"), ...);
  }(std::make_index_sequence<SyscallMeta::kOutputNames.size()>{});

  return "{\n" + out + "}";
}

inline std::string
syscall_error_message(HostFnStatus<SyscallErrorCode> status)
{
  if (!status.has_value()) {
    switch (status.error().code()) {
    case HostFnErrorCode::Exit:
      return "Host Exit (Contract Execution Panic)\n";
      break;
    case HostFnErrorCode::Return:
      return "Host Return (Contract Execution Success)\n";
      break;
    default:
      return std::format("Host Error ({})\n", x23::show(status.error().code()));
    }
  }
  assert(*status != SyscallErrorCode::kSyscallErrorCodeNone);
  return std::format("Syscall Error: {}", x23::show(*status));
}

template<typename T>
auto
to_xdr_trace(T const &val)
{
  if constexpr (std::same_as<T, std::span<const std::byte>> ||
                std::same_as<T, std::span<std::byte>>) {
    x23::Vec<uint8_t> out;
    if (!val.empty()) {
      out.append(reinterpret_cast<const uint8_t *>(val.data()), val.size());
    }
    return out;
  }
  else if constexpr (is_std_optional<T>) {
    using value_t = std::remove_cvref_t<typename T::value_type>;
    if (val) {
      return x23::Ptr<value_t>(new value_t(to_xdr_trace(*val)));
    }
    return x23::Ptr<value_t>();
  }
  else if constexpr (std::is_pointer_v<T>) {
    if (val) {
      return x23::Ptr<decltype(to_xdr_trace(*val))>(
          new decltype(to_xdr_trace(*val))(to_xdr_trace(*val)));
    }
    return x23::Ptr<decltype(to_xdr_trace(*val))>();
  }
  else if constexpr (is_ffi_helper_required<T>) {
    return FFIHelperType<T>::to_cpp(val);
  }
  else if constexpr (std::same_as<T, int128_t> || std::same_as<T, uint128_t>) {
    return to_xdr(val);
  }
  else {
    return val;
  }
}

template<typename Target, typename... Args>
void
set_syscall_trace_inputs(Target &target, std::tuple<Args...> const &parsed_args)
{
  [&]<size_t... Is>(std::index_sequence<Is...> /*unused*/) {
    ((get<Is>(target) = to_xdr_trace(std::get<Is>(parsed_args))), ...);
  }(std::make_index_sequence<std::tuple_size_v<Target>>{});
}

template<typename T>
void
disarm_output(T &output)
{
  if constexpr (requires { output.disarm(); }) {
    output.disarm();
  }
}

template<typename... Args>
void
disarm_outputs(std::tuple<Args...> &args)
{
  [&]<size_t... Is>(std::index_sequence<Is...> /*unused*/) {
    (disarm_output(std::get<Is>(args)), ...);
  }(std::make_index_sequence<std::tuple_size_v<std::tuple<Args...>>>{});
}

// report_dropped_syscall_trace() lives in syscall_types.h so the per-syscall
// inner bodies (which also materialize optional XDR trace outputs and must not
// panic on allocation failure) can share it.

// This logic -- validating all of the syscall inputs, calling the inner
// syscall, logging, read-only checks, and returning the result -- is common to
// both the regular and mock syscall handlers.  This is split out from
// wrap_outer so as to minimize code duplication (and reduce the chance of an
// accidental discrepancy between the test framework and the actual runtime).
//
// Passes in the syscall number so that tracing can handle invalid syscall
// numbers.
template<is_syscall_meta SyscallMeta>
constexpr std::pair<HostFnStatus<SyscallErrorCode>, GasAmount>
wrap_after_parse(AliasGuard &guard, SystemCallHandler &handler,
                 const GasAmount gas_remaining, SyscallNo syscall_no,
                 auto &&args)
{
  using syscall_t =
      SyscallTypes<std::remove_const_t<decltype(SyscallMeta::kInnerFunc)>>;
  constexpr static SyscallNo kSyscall = SyscallMeta::kSyscallNo;

  handler.start_syscall_trace(syscall_no, gas_remaining);

  // Wrapping the logic in a lambda so that no matter what, the checks at the
  // end to consume gas and validate return state cannot be skipped.
  auto [syscall_result, gas_consumed] =
      [&]<size_t... Is>(std::index_sequence<Is...> /*unused*/) noexcept
      -> std::pair<HostFnStatus<SyscallErrorCode>, WrappedGasAmount> {
    // check whether any loads failed
    if (!(SyscallArgParams<typename syscall_t::template arg_i<Is>>::validate(
              std::get<Is>(args)) &&
          ...)) {
      if (auto *stats = handler.get_transaction_stats()) {
        stats->syscall_fault_count++;
      }
      disarm_outputs(args);
      return kFaultResult;
    }

    // Check whether any inputs/outputs were aliased
    if (!guard.check()) {
      try {
        trace("Syscall {} faulted due to aliased memory regions", kSyscall);
      } catch (std::exception const &e) {
        report_dropped_syscall_trace(e.what());
      }
      disarm_outputs(args);
      return kFaultResult;
    }

    if constexpr (!SyscallMeta::kIsReadOnlySyscall) {
      if (handler.is_read_only()) {
        try {
          trace("Syscall {} is not read-only, but called in read-only mode",
                kSyscall);
        } catch (std::exception const &e) {
          report_dropped_syscall_trace(e.what());
        }
        disarm_outputs(args);
        return {HostFnStatus<SyscallErrorCode>{
                    SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput},
                WrappedGasAmount{0}};
      }
    }

    if (runtime_log_level(LogLevel::kTrace)) {
      // Once the syscall inputs are parsed, we can print them in a formatted
      // log message. Formatting allocates, so keep it best-effort: this runs in
      // the noexcept syscall body and must not crash on allocation failure
      // (issue #2562).
      try {
        trace(
            "Syscall {} Input: {}", kSyscall,
            make_syscall_input_message<SyscallMeta>(std::tuple{
                SyscallArgParams<typename syscall_t::template arg_i<Is>>::deref(
                    std::get<Is>(args))...}));
      } catch (std::exception const &e) {
        report_dropped_syscall_trace(e.what());
      }
    }

    if constexpr (SyscallMeta::kSyscallNo != kNoTracingSyscall) {
      if (auto *trace_data =
              handler.get_syscall_trace_data<SyscallMeta::kSyscallNo>()) {
        // Copying inputs into the XDR trace record allocates and can throw.
        // Keep it best-effort rather than panicking: this runs in the noexcept
        // syscall body, so a thrown std::bad_alloc would terminate an honest
        // caller before the mock entrypoint's catch could translate it. Drop
        // the trace inputs and continue (issue #2561).
        try {
          [&]<size_t... Inputs>(std::index_sequence<Inputs...> /*unused*/) {
            set_syscall_trace_inputs(
                trace_data->inputs,
                std::tuple{SyscallArgParams<typename syscall_t::template arg_i<
                    Inputs>>::deref(std::get<Inputs>(args))...});
          }(std::make_index_sequence<
              std::tuple_size_v<decltype(trace_data->inputs)>>{});
        } catch (std::exception const &e) {
          report_dropped_syscall_trace(e.what());
        }
      }
    }

    auto extra_args =
        syscall_t::extra_args(handler, WrappedGasAmount{gas_remaining});

    // Invoke the inner syscall
    // template only required because we can't do auto [...handler] =
    // handler_pack; yet.  This would be so much cleaner in c++26.
    //
    // The inner syscall bodies run arbitrary allocating storage/cache/frame
    // logic against attacker-controlled inputs, so this is the exception
    // boundary for all of them: anything that escapes (most commonly
    // std::bad_alloc under memory pressure) is translated into a structured
    // Unrecoverable result here instead of terminating hogexec (issue #3400
    // and siblings). The inner bodies are deliberately not noexcept so the
    // translation happens here instead of at their own boundary via
    // std::terminate(); this lambda is noexcept so that boundary is
    // reestablished immediately around the catch.
    auto res = [&]<size_t... ExtraArgs>(
                   std::index_sequence<ExtraArgs...> /*unused*/) noexcept
        -> std::pair<HostFnStatus<SyscallErrorCode>, WrappedGasAmount> {
      try {
        return SyscallMeta::kInnerFunc(
            std::get<ExtraArgs>(extra_args)...,
            SyscallArgParams<typename syscall_t::template arg_i<Is>>::deref(
                std::get<Is>(args))...);
      } catch (std::exception const &e) {
        // The diagnostic itself allocates and could throw again (issue
        // #3372); report through the no-allocation fallback if it does.
        try {
          error("unrecoverable: syscall {} body threw: {}", kSyscall, e.what());
        } catch (...) {
          report_dropped_syscall_trace("syscall-body exception diagnostic");
        }
        disarm_outputs(args);
        return {HostFnError{HostFnErrorCode::Unrecoverable},
                WrappedGasAmount{0}};
      } catch (...) {
        report_dropped_syscall_trace("non-std exception from syscall body");
        disarm_outputs(args);
        return {HostFnError{HostFnErrorCode::Unrecoverable},
                WrappedGasAmount{0}};
      }
    }(std::make_index_sequence<std::tuple_size_v<decltype(extra_args)>>{});

    if (runtime_log_level(LogLevel::kTrace)) {
      // The inner syscall has already run and written its outputs; formatting
      // them for the trace log allocates. Keep it best-effort so an allocation
      // failure here does not terminate the noexcept syscall body after a
      // successful side effect (issue #2562).
      try {
        bool is_syscall_error =
            !res.first.has_value() ||
            (*res.first) != SyscallErrorCode::kSyscallErrorCodeNone;
        trace("Syscall {} Output: {}", kSyscall,
              is_syscall_error
                  ? syscall_error_message(res.first)
                  : make_syscall_output_message<SyscallMeta>(std::tuple{
                        SyscallArgParams<typename syscall_t::template arg_i<
                            Is>>::deref(std::get<Is>(args))...}));
      } catch (std::exception const &e) {
        report_dropped_syscall_trace(e.what());
      }
    }

    return res;
  }(std::make_index_sequence<syscall_t::kNumArgs>{});

  if constexpr (is_debug_build()) {
    // In debug builds, verify that the syscall only returns documented error
    // codes. This helps catch cases where a syscall returns an error that isn't
    // documented in syscalls.yaml.
    if (syscall_result.has_value() &&
        !is_allowed_error_code<SyscallMeta>(*syscall_result)) {
      panic("Syscall {} returned undocumented error code {}",
            SyscallMeta::kSyscallNo, x23::show(*syscall_result));
    }
  }

  // Here we perform gas accounting.  Errors that might be handled by a contract
  // (via a parent frame) still need to have gas accounting performed.  Errors
  // that fail the whole transaction cannot be accidentally turned into an out
  // of gas error.
  bool is_handleable_error = [&]() noexcept {
    if (!syscall_result.has_value()) {
      switch (syscall_result.error().code()) {
      case HostFnErrorCode::Abandon:
      case HostFnErrorCode::ValidationFailure:
        return false;
      case HostFnErrorCode::Unrecoverable:
        try {
          error("Unrecoverable: syscall_entry_outer got Unrecoverable");
        } catch (std::exception const &e) {
          report_dropped_syscall_trace(e.what());
        }
        return false;
      case HostFnErrorCode::OutOfGas:
      case HostFnErrorCode::Exit:
      case HostFnErrorCode::Return:
        return true;
      case HostFnErrorCode::NoneOrRecoverable:
        panic("unreachable");
      }
      panic("Unhandled HostFnErrorCode (invalid enum?)");
    }
    return true;
  }();

  syscall_result = [&]() noexcept -> HostFnStatus<SyscallErrorCode> {
    if (is_handleable_error) {
      if (gas_consumed.get() >= gas_remaining) {
        return {HostFnError{HostFnErrorCode::OutOfGas}};
      }
    }

    // The syscall_result.has_value() check means that we only
    // override syscall_result (by returning Exit) in a subset
    // of the cases covered by the is_handleable_error guard.
    if (!SyscallMeta::kCanReturn && syscall_result.has_value()) {
      // If the syscall cannot return to the wasm environment but the syscall
      // tried to return an error code, abort the entire call frame.
      if (runtime_log_level(LogLevel::kTrace)) {
        try {
          trace("Syscall {} attempted to return an error code {} but cannot "
                "return",
                SyscallMeta::kSyscallNo, x23::show(*syscall_result));
        } catch (std::exception const &e) {
          report_dropped_syscall_trace(e.what());
        }
      }
      return {HostFnError{HostFnErrorCode::Exit}};
    }

    if (syscall_result.has_value() &&
        *syscall_result != SyscallErrorCode::kSyscallErrorCodeNone) {
      disarm_outputs(args);
    }
    return syscall_result;
  }();

  GasAmount gas_remaining_at_end = gas_remaining > gas_consumed.get()
                                       ? gas_remaining - gas_consumed.get()
                                       : 0;
  handler.finish_syscall_trace(syscall_result, gas_remaining_at_end);

  return {syscall_result,
          std::min<GasAmount>(gas_consumed.get(), gas_remaining)};
}

// Wraps a call to an inner syscall with checks to load syscall arguments
// from contract memory and check that all accesses into contract memory
// are well-formed (no OOB).
template<is_syscall_meta SyscallMeta>
constexpr std::pair<HostFnStatus<SyscallErrorCode>, GasAmount>
wrap_outer(SystemCallHandler &handler, WasmContext wasm_context,
           SyscallNo syscall_no, uint32_t arg1, uint32_t arg2, uint32_t arg3,
           uint32_t arg4, uint32_t arg5, uint32_t arg6, uint32_t arg7) noexcept
{
  using syscall_t =
      SyscallTypes<std::remove_const_t<decltype(SyscallMeta::kInnerFunc)>>;

  // syscall_entry carries arg0 (syscall number) plus seven argument words
  // (arg1..arg7) for both the WASM trampoline and the RV32 register-native
  // bridge. A syscall that needs more register words than that cannot be
  // dispatched; pack extra scalars into a struct passed by guest pointer.
  static_assert(syscall_t::kTotalInputs <= 7,
                "syscall consumes more than 7 register argument words");

  auto input_args = std::tuple{arg1, arg2, arg3, arg4, arg5, arg6, arg7};

  std::span<std::byte> contract_memory_span(reinterpret_cast<std::byte *>(
                                                wasm_context.memory.mem),
                                            wasm_context.memory.sz);

  // Again, parameter packs as structured bindings gets rid of the annoying
  // lambda context
  auto res = [&]<size_t... Is>(TupleIndexSequence<Is...> /*unused*/) noexcept
      -> std::pair<HostFnStatus<SyscallErrorCode>, GasAmount> {
    // Attempt to load syscall arguments from contract memory.
    AliasGuard guard;
    auto parsed_args = std::tuple{parse<syscall_t::template offset_i<Is>(),
                                        typename syscall_t::template arg_i<Is>>(
        guard, contract_memory_span, input_args)...};

    return wrap_after_parse<SyscallMeta>(guard, handler,
                                         wasm_context.gas_remaining, syscall_no,
                                         std::move(parsed_args));
  }(from_index_sequence(std::make_index_sequence<syscall_t::kNumArgs>{}));

  return res;
}

} // namespace hogd
