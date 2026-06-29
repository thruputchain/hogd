#include "hogd/hogexec/runtime/rv32/runtime_context.h"
#include "hogd/hogexec/runtime/item/deployed_contract.h"

#include <cassert>
#include <memory>
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
#include "types.hh"

namespace hogd::rv32 {
namespace {

InvokeStatus<void>
host_trap_code_to_invoke_status(uint32_t code)
{
  switch (static_cast<HostFnErrorCode>(code)) {
  case HostFnErrorCode::Unrecoverable:
    return InvokeError{InvokeErrorCode::Unrecoverable};
  case HostFnErrorCode::ValidationFailure:
    return InvokeError{InvokeErrorCode::ValidationFailure};
  case HostFnErrorCode::Abandon:
    return InvokeError{InvokeErrorCode::Abandon};
  case HostFnErrorCode::OutOfGas:
  case HostFnErrorCode::Return:
  case HostFnErrorCode::Exit:
  case HostFnErrorCode::NoneOrRecoverable:
    break;
  }

  // Only fatal-class host trap codes mirror the WASM mapping here. Return,
  // Exit, and OutOfGas are handled before HostTrap by SyscallBridgeHost and the
  // VM trap/halt status; seeing them as trap payloads is a fatal inconsistency.
  return InvokeError{InvokeErrorCode::Unrecoverable};
}

bool
runtime_contract_image_is_valid(const RuntimeContractImage &image)
{
  // memory_bytes is a free uint32_t on the image; cap it at kMaxMemoryBytes so
  // an oversized host config is rejected here as invalid rather than surfacing
  // later as a per-invocation construction trap misreported as a guest fault.
  // memory_bytes == 0 is allowed (a zero-memory contract is a valid degenerate
  // image; initial_memory must then be empty, which the size check enforces).
  return image.program != nullptr && image.memory_bytes <= kMaxMemoryBytes &&
         image.initial_memory.size() <= image.memory_bytes;
}

class Runtime final : public hogd::Runtime {
public:
  Runtime(RuntimeContractImage image, SystemCallHandler *syscall_handler)
    : hogd::Runtime(syscall_handler), image_(std::move(image)),
      image_is_valid_(runtime_contract_image_is_valid(image_))
  {}

protected:
  std::pair<InvokeStatus<void>, GasAmount>
  invoke_internal(MethodID method_id, uint64_t gas_limit) override
  {
    if (!image_is_valid_) {
      return {InvokeError{InvokeErrorCode::Unrecoverable}, 0};
    }

    // v4: enter at PC 0 (_start) and seed the invoked method id into a0/a1;
    // the guest dispatches. An unknown method is handled in-guest (the
    // dispatcher's default arm), not by a host-side pre-execution miss.
    Machine machine(image_.program, image_.memory_bytes, gas_limit);
    if (!image_.initial_memory.empty() &&
        !machine.view().write_memory(0, image_.initial_memory)) {
      return {InvokeError{InvokeErrorCode::Unrecoverable}, 0};
    }
    machine.seed_method_id(method_id);

    SyscallBridgeHost host(syscall_handler_, image_.entry == nullptr
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
        if (machine.host_trap_kind() == HostTrapKind::kDeterministic) {
          return {InvokeError{InvokeErrorCode::Deterministic}, gas_consumed};
        }
        return {host_trap_code_to_invoke_status(machine.host_trap_code()),
                gas_consumed};
      case Trap::kInvalidInstruction:
      case Trap::kBadPc:
      case Trap::kBadMemory:
      case Trap::kBreakpoint:
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

private:
  RuntimeContractImage image_;
  const bool image_is_valid_;
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
  if (it == contracts_.end() || !runtime_contract_image_is_valid(it->second)) {
    return InvokeErrorCode::Deterministic;
  }
  return InvokeErrorCode::None;
}

SystemStatus<std::unique_ptr<hogd::Runtime>>
RuntimeContext::make_new_runtime(DeployedContract const &deployed_contract,
                                 SystemCallHandler *syscall_handler)
{
  DeployedContractHash const &deployed_contract_hash = deployed_contract.hash;
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

  return std::make_unique<Runtime>(it->second, syscall_handler);
}

} // namespace hogd::rv32
