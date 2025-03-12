#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>

#include "contract.hh"
#include "hogd/base/types.h"
#include "hogd/hogexec/decl/lficpp.h"
#include "hogd/hogexec/decl/sandbox_cache.h"
#include "hogd/hogexec/runtime/runtime.h"
#include "hogd/hogexec/runtime/status.h"

namespace hogd {

class DeclContext : public RuntimeContext {
public:
  DeclContext();

  ~DeclContext() override;

  std::unique_ptr<Runtime> make_new_runtime(ContractBlob const &contract,
                                            SystemCallHandler *syscall_handler,
                                            bool do_contract_verify) override;

  bool verify_contract(ContractBlob const &contract) const override;

private:
  bool link_fn_nargs(std::string_view module_name, std::string_view fn_name,
                     void *fn_ptr, uint8_t nargs) override;

  SandboxCache sandbox_cache_;
};

// Wrapper around one Wasm module (one wasmi instance). Can be invoked, and
// will call into provided host functions and pass in (as an argument) the
// provided SystemCallHandler.
class DeclRuntime : public Runtime {
public:
  DeclRuntime(std::unique_ptr<HostCallContext> host_call_context,
               std::unique_ptr<DeclProc> proc, SandboxCache& parent_cache);

  ~DeclRuntime() override;

  [[nodiscard]] uint64_t get_available_gas() const noexcept override;
  void set_available_gas(uint64_t gas) noexcept override;

  // Complex data types pass in/out of wasm via read/writes to wasm linear
  // memory.
  Memory get_memory() noexcept override;

private:
  InvokeStatus<int32_t>
  invoke_internal(std::string const &method_name) override;

  SandboxCache& parent_cache_;
  std::unique_ptr<DeclProc> proc_; 
};

} // namespace hogd
