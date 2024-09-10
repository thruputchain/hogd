#pragma once

#include "hogd/runtime/host_fn_status.h"

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace hogd {

class WasmiRuntime;

struct HostCallContext {
  WasmiRuntime *runtime = nullptr;
  void *user_ctx = nullptr;
};

// placeholder types until XDR gets merged
using Contract = std::vector<uint8_t>;

namespace detail {

template<typename... T> struct ArgsCounter;

template<> struct ArgsCounter<> {
  constexpr static uint8_t kNArgs = 0;
};

template<typename... T> struct ArgsCounter<uint64_t, T...> {
  constexpr static uint8_t kNArgs = 1 + ArgsCounter<T...>::kNArgs;
};

template<typename... Args>
constexpr uint8_t
get_n_args(HostFnStatus<uint64_t> (*)(HostCallContext *, Args...))
{
  return ArgsCounter<Args...>::kNArgs;
}

} // namespace detail

struct InvokeResult {
  // topic for debate: should result be a different error status type
  // or does it not matter
  HostFnStatus<uint64_t> result;
  uint64_t gas_consumed;
};

class WasmiRuntime  {
public:
  WasmiRuntime(Contract const &contract, void *user_ctx_ptr);

  WasmiRuntime(WasmiRuntime const&) = delete;

  template<auto F>
  bool link_fn(std::string const &module_name, std::string const &fn_name)
  {
    return link_fn_nargs(module_name, fn_name, reinterpret_cast<void *>(F),
                         detail::get_n_args(F));
  }

  InvokeResult invoke(std::string const &method_name, uint64_t gas_limit);

  // Returns FALSE if gas consumption failed
  [[nodiscard]]
  bool consume_gas(uint64_t gas) noexcept;

  [[nodiscard]] uint64_t get_available_gas() const;
  void set_available_gas(uint64_t gas);

  std::span<uint8_t, std::dynamic_extent> get_memory() noexcept;
  std::span<const uint8_t, std::dynamic_extent> get_memory() const noexcept;

  ~WasmiRuntime();

private:
  bool link_fn_nargs(std::string const &module_name, std::string const &fn_name,
                     void *fn_ptr, uint8_t nargs);

  HostCallContext host_call_context_;
  // manipulated in rust
  void *wasmi_runtime_ptr_;
};

} // namespace hogd
