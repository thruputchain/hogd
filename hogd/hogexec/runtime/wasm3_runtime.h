#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "contract.hh"
#include "hogd/hogexec/runtime/runtime.h"
#include "hogd/hogexec/runtime/status.h"
#include "hogd/hogexec/runtime/wasm3_wrapper.h"
#include "types.hh"

namespace hogd {

// Context to be passed into a host function call
class Wasm3Runtime;

class Wasm3Context : public RuntimeContext {
public:
  Wasm3Context() = default;
  ~Wasm3Context() override = default;

  std::unique_ptr<Runtime>
  make_new_runtime(ContractBlob const &contract,
                   SystemCallHandler *syscall_handler) override;

private:
  struct LinkedFunction {
    std::string module_name_;
    std::string fn_name_;
    void *fn_pointer_;
    uint8_t nargs_;
  };

  bool link_fn_nargs(std::string_view module_name, std::string_view fn_name,
                     void *fn_ptr, uint8_t nargs) override
  {
    functions_to_link_.emplace_back(std::string(module_name),
                                    std::string(fn_name), fn_ptr, nargs);
    return true;
  }

  std::vector<LinkedFunction> functions_to_link_;
  wasm3::environment env;
};

// Wrapper around one Wasm module (one wasmi instance). Can be invoked, and
// will call into provided host functions and pass in (as an argument) the
// provided SystemCallHandler.
class Wasm3Runtime : public Runtime {
public:
  // should be called only from within WasmiContext::make_new_runtime
  Wasm3Runtime(std::unique_ptr<HostCallContext> host_call_context,
               std::unique_ptr<wasm3::module> wasm3_module,
               std::unique_ptr<wasm3::runtime> wasm3_runtime)
    : Runtime(std::move(host_call_context)),
      wasm3_module_(std::move(wasm3_module)),
      wasm3_runtime_(std::move(wasm3_runtime))
  {}

  ~Wasm3Runtime() override {};

  [[nodiscard]] uint64_t get_available_gas() const noexcept override;
  void set_available_gas(uint64_t gas) noexcept override;

  // Complex data types pass in/out of wasm via read/writes to wasm linear
  // memory.
  //
  // Requires the module to export its memory as "memory"
  // (export "memory" (memory 0))
  std::span<std::byte> get_memory() noexcept override;
  std::span<const std::byte> get_memory() const noexcept override;

private:
  InvokeStatus<int32_t> invoke_internal(std::string_view method_name) override;

  std::unique_ptr<wasm3::module> wasm3_module_;
  std::unique_ptr<wasm3::runtime> wasm3_runtime_;
  uint64_t available_gas_ = 0;
};

} // namespace hogd
