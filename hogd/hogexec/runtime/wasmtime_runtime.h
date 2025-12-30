#pragma once

#include <cstddef>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "account.hh"
#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h" // IWYU pragma: keep
#include "hogd/hogexec/runtime/runtime.h"
#include "hogd/hogexec/runtime/status.h"
#include "types.hh"

namespace hogd {

struct Contract;

class WasmtimeModuleCache {
public:
  void put(SHA256Hash const &hash, std::span<uint8_t const> module)
  {
    std::lock_guard<std::mutex> lock(mtx_);
    std::vector<uint8_t> module_copy(module.begin(), module.end());
    module_cache_.insert({hash, std::move(module_copy)});
  }

  std::optional<std::span<uint8_t>> get(SHA256Hash const &hash)
  {
    std::lock_guard<std::mutex> lock(mtx_);
    auto it = module_cache_.find(hash);
    if (it == module_cache_.end()) {
      return std::nullopt;
    }
    return std::span<uint8_t>(it->second.data(), it->second.size());
  }

private:
  // TODO(geoff): better impl
  std::mutex mtx_;
  std::unordered_map<SHA256Hash, std::vector<uint8_t>> module_cache_;
};

class WasmtimeGlobalContext {
public:
  explicit WasmtimeGlobalContext(size_t engine_count);
  ~WasmtimeGlobalContext();

  void *get_inner(size_t engine_index) const;
  size_t alloc_engine_index();

  WasmtimeModuleCache &module_cache() { return module_cache_; }

private:
  std::vector<void *> engines_;
  WasmtimeModuleCache module_cache_;
};

// Context shared between WasmtimeRuntime objects.
// Guarantee isolation between transactions
// by starting each transaction with a separate
// WasmtimeContext.  Role is to link syscall trampolines
// and provide new runtime instances.
//
// The intent with this API is to abstract away most of the details
// of wasmtime, so that later switching to a different engine, should we so
// choose, should not require significant redesign work.
//
// This should NOT be used across multiple concurrent transactions.
class WasmtimeContext : public RuntimeContext {
public:
  // module_cache can be nullptr, in which case no module cache is used.
  // module_cache must outlive this WasmtimeContext.
  WasmtimeContext(WasmtimeGlobalContext &global_context);

  // wrapper around a manual drop call on the rust side
  ~WasmtimeContext() override;

  SystemStatus<std::unique_ptr<Runtime>>
  make_new_runtime(DeployedContractHash const &deployed_contract_hash,
                   SystemCallHandler *syscall_handler,
                   bool do_contract_verify) override;

  bool verify_contract(Contract const &contract) const override;

private:
  bool link_fn_nargs(std::string_view module_name, std::string_view fn_name,
                     void *fn_ptr, uint8_t nargs) override;

  size_t engine_index_;
  void *wasmtime_context_ptr_ = nullptr;
};

// Wrapper around one Wasm module (one wasmtime instance). Can be invoked, and
// will call into provided host functions and pass in (as an argument) the
// provided SystemCallHandler.
class WasmtimeRuntime : public Runtime {
public:
  // should be called only from within WasmtimeContext::make_new_runtime
  WasmtimeRuntime(std::unique_ptr<HostCallContext> host_call_context,
                  void *wasmtime_runtime_ptr);

  ~WasmtimeRuntime() override;

  [[nodiscard]] uint64_t get_available_gas() const noexcept override;
  void set_available_gas(uint64_t gas) noexcept override;

  // Complex data types pass in/out of wasm via read/writes to wasm linear
  // memory.
  //
  // Requires the module to export its memory as "memory"
  // (export "memory" (memory 0))
  std::span<std::byte> get_memory() noexcept override;

private:
  InvokeStatus<void> invoke_internal(std::string_view method_name) override;

  // manipulated in rust
  void *wasmtime_runtime_ptr_;
};

} // namespace hogd
