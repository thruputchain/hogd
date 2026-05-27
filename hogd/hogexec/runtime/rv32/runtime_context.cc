#include "hogd/hogexec/runtime/rv32/runtime_context.h"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <memory>
#include <string_view>
#include <unordered_map>
#include <utility>

#include "contract.hh"
#include "ffi_enums.hh"
#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h"
#include "hogd/hogexec/common/status.h"
#include "hogd/hogexec/runtime/runtime.h"
#include "hogd/hogexec/runtime/rv32/host.h"
#include "hogd/hogexec/runtime/rv32/machine.h"
#include "hogd/hogexec/runtime/rv32/program.h"
#include "hogd/hogexec/runtime/rv32/syscall_bridge.h"
#include "hogd/hogexec/runtime/syscall_entry.h"
#include "hogd/hogexec/runtime/syscall_handler.h"

namespace hogd::rv32 {
namespace {

InvokeStatus<void>
host_trap_code_to_invoke_status(uint32_t code)
{
  switch (static_cast<HostFnErrorCode>(code)) {
  case HostFnErrorCode::OutOfGas:
    return InvokeError{InvokeErrorCode::OutOfGas};
  case HostFnErrorCode::Return:
    return {};
  case HostFnErrorCode::Exit:
    return InvokeError{InvokeErrorCode::Deterministic};
  case HostFnErrorCode::Unrecoverable:
    return InvokeError{InvokeErrorCode::Unrecoverable};
  case HostFnErrorCode::ValidationFailure:
    return InvokeError{InvokeErrorCode::ValidationFailure};
  case HostFnErrorCode::Abandon:
    return InvokeError{InvokeErrorCode::Abandon};
  case HostFnErrorCode::NoneOrRecoverable:
    break;
  }

  // A host trap with no fatal host code, or with an unknown code, is still a
  // fatal runtime outcome. Known fatal host codes above mirror the WASM
  // mapping.
  return InvokeError{InvokeErrorCode::Unrecoverable};
}

class Runtime final : public hogd::Runtime {
public:
  Runtime(RuntimeContractImage image,
          std::unique_ptr<HostCallContext> host_call_context)
    : hogd::Runtime(std::move(host_call_context)), image_(std::move(image))
  {}

private:
  std::pair<InvokeStatus<void>, GasAmount>
  invoke_internal(std::string_view method_name, uint64_t gas_limit) override
  {
    auto method_id = hogd::Runtime::method_id_from_name(method_name);
    if (!method_id) {
      error("Unrecoverable: rv32::Runtime failed to parse method name '{}'",
            method_name);
      return {InvokeError{InvokeErrorCode::Unrecoverable}, 0};
    }
    if (*method_id != 0) {
      // M3 exposes one RV32 test entry point. Named methods become a stable
      // deterministic miss until M4 adds contract-defined dispatch.
      return {InvokeError{InvokeErrorCode::Deterministic}, 0};
    }
    if (image_.program == nullptr ||
        image_.initial_memory.size() > image_.memory_bytes) {
      return {InvokeError{InvokeErrorCode::Unrecoverable}, 0};
    }

    Machine machine(image_.program, image_.memory_bytes, gas_limit);
    if (!image_.initial_memory.empty() &&
        !machine.view().write_memory(0, image_.initial_memory)) {
      return {InvokeError{InvokeErrorCode::Unrecoverable}, 0};
    }

    SyscallBridgeHost host(host_call_context_.get(), image_.entry == nullptr
                                                         ? &syscall_entry
                                                         : image_.entry);
    const StepResult step_result = machine.run(host);
    const GasAmount gas_consumed = gas_limit - machine.gas_remaining();

    switch (step_result) {
    case StepResult::kHalted:
      if (machine.halt_code() == 0) {
        return {{}, gas_consumed};
      }
      return {InvokeError{InvokeErrorCode::Deterministic}, gas_consumed};
    case StepResult::kTrap:
      switch (machine.trap()) {
      case Trap::kOutOfGas:
        return {InvokeError{InvokeErrorCode::OutOfGas}, gas_consumed};
      case Trap::kHostTrap:
        return {host_trap_code_to_invoke_status(machine.host_trap_code()),
                gas_consumed};
      case Trap::kInvalidInstruction:
      case Trap::kBadPc:
      case Trap::kBadMemory:
      case Trap::kMisalignedAccess:
        return {InvokeError{InvokeErrorCode::Deterministic}, gas_consumed};
      case Trap::kNone:
        CHECK(false);
        return {InvokeError{InvokeErrorCode::Unrecoverable}, gas_consumed};
      }
      break;
    case StepResult::kContinue:
    case StepResult::kEcall:
      CHECK(false);
      return {InvokeError{InvokeErrorCode::Unrecoverable}, gas_consumed};
    }

    return {InvokeError{InvokeErrorCode::Unrecoverable}, gas_consumed};
  }

  RuntimeContractImage image_;
};

} // namespace

RuntimeContext::RuntimeContext(
    std::unordered_map<int32_t, RuntimeContractImage> contracts)
  : contracts_(std::move(contracts))
{}

InvokeErrorCode
RuntimeContext::verify_contract(Contract const &contract) const
{
  if (contract.contract_type() != ContractType::kMockTest) {
    return InvokeErrorCode::Deterministic;
  }
  const int32_t contract_id = contract.mock_contract_id();
  auto it = contracts_.find(contract_id);
  if (it == contracts_.end() || it->second.program == nullptr ||
      it->second.initial_memory.size() > it->second.memory_bytes) {
    return InvokeErrorCode::Deterministic;
  }
  return InvokeErrorCode::None;
}

SystemStatus<std::unique_ptr<hogd::Runtime>>
RuntimeContext::make_new_runtime(
    DeployedContractHash const &deployed_contract_hash,
    SystemCallHandler *syscall_handler)
{
  if (deployed_contract_hash.contract_type != ContractType::kMockTest) {
    error("Unrecoverable: Attempt made to access a non-mock-test contract from "
          "rv32::RuntimeContext: {}",
          deployed_contract_hash.contract_type);
    return SystemError{SystemErrorCode::Unrecoverable};
  }
  assert(syscall_handler != nullptr);

  auto contract =
      syscall_handler->resolve_contract(deployed_contract_hash.hash);
  if (!contract.has_value()) [[unlikely]] {
    return contract.error();
  }
  if (contract.value() == nullptr) [[unlikely]] {
    error("Unrecoverable: Contract lookup failed: {}",
          show_byte_array(deployed_contract_hash.hash));
    return SystemError{SystemErrorCode::Unrecoverable};
  }
  if (contract.value()->contract_type() != deployed_contract_hash.contract_type)
      [[unlikely]] {
    error("Unrecoverable: Mismatch in contract type: {} != {}",
          contract.value()->contract_type(),
          deployed_contract_hash.contract_type);
    return SystemError{SystemErrorCode::Unrecoverable};
  }

  const int32_t contract_id = contract.value()->mock_contract_id();
  auto it = contracts_.find(contract_id);
  if (it == contracts_.end()) {
    warn("Attempt made to access an RV32 test contract that is not defined: {}",
         contract_id);
    return nullptr;
  }

  return std::make_unique<Runtime>(it->second,
                                   make_new_host_call_context(syscall_handler));
}

} // namespace hogd::rv32
