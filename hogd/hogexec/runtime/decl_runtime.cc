#include "hogd/hogexec/runtime/decl_runtime.h"

#include <cassert>
#include <memory>
#include <string>
#include <utility>

#include "contract.hh"
#include "hogd/base/types.h"
#include "hogd/hogexec/decl/lficpp.h"
#include "hogd/hogexec/decl/sandbox_cache.h"
#include "hogd/hogexec/runtime/runtime.h"
#include "hogd/hogexec/runtime/status.h"


namespace hogd {

DeclRuntime::DeclRuntime(std::unique_ptr<HostCallContext> host_call_context,
               std::unique_ptr<DeclProc> proc, SandboxCache& parent_cache)
  : Runtime(std::move(host_call_context))
  , parent_cache_(parent_cache)
  , proc_(std::move(proc)) {}

DeclRuntime::~DeclRuntime()
{
    parent_cache_.return_proc(std::move(proc_));
}

uint64_t 
DeclRuntime::get_available_gas() const noexcept {
    return proc_->get_available_gas();
}

void 
DeclRuntime::set_available_gas(uint64_t gas) noexcept {
    proc_->set_available_gas(gas);
};

Memory 
DeclRuntime::get_memory() noexcept {
    return proc_->get_memory();
}

InvokeStatus<int32_t>
DeclRuntime::invoke_internal(std::string const &method_name) 
{
    // TODO(geoff) calldata? and function number
    auto method_num = Runtime::method_id_from_string(method_name);
    assert(method_num.has_value());

    return proc_->invoke(*method_num);
}

bool 
DeclContext::verify_contract(ContractBlob const &contract) const {
    return verify_contract_bytes(contract);
}





}