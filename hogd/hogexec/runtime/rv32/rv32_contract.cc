#include "hogd/hogexec/runtime/rv32/rv32_contract.h"
#include "hogd/hogexec/runtime/item/deployed_contract.h"

#include <cassert>
#include <exception>
#include <expected>
#include <memory>
#include <optional>
#include <utility>

#include "contract.hh"
#include "ffi_enums.hh"
#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h"
#include "hogd/hogexec/common/status.h"
#include "hogd/hogexec/runtime/item/storage_snapshot_cache.h"
#include "hogd/hogexec/runtime/runtime.h"
#include "hogd/hogexec/runtime/runtime_exports.h"
#include "hogd/hogexec/runtime/rv32/artifact.h"
#include "hogd/hogexec/runtime/rv32/host.h"
#include "hogd/hogexec/runtime/rv32/threaded_machine.h"
#include "hogd/hogexec/runtime/syscall_entry.h"
#include "hogd/hogexec/runtime/syscall_handler.h"
#include "types.hh"

namespace hogd {

std::optional<Rv32Contract>
Rv32Contract::create(ContractBlob const &contract_blob)
{

  auto artifact = rv32::parse_threaded_artifact(contract_blob.data);
  if (!artifact.has_value()) {
    return std::nullopt;
  }

  auto pre_instantiation = rv32::ThreadedMachinePreInstantiation::create(
      artifact->code, artifact->initial_memory, artifact->memory_bytes, false);
  if (!pre_instantiation.has_value()) {
    return std::nullopt;
  }

  return std::make_optional<Rv32Contract>(std::move(*pre_instantiation));
}

Rv32Contract::Rv32Contract(
    rv32::ThreadedMachinePreInstantiation pre_instantiation)
  : pre_instantiation_(std::move(pre_instantiation))
{}

rv32::ThreadedMachine
Rv32Contract::instantiate(uint64_t gas) const
{
  return rv32::ThreadedMachine(pre_instantiation_, gas);
}

namespace {

std::pair<std::expected<int32_t, HostFnErrorCode>, GasAmount>
from_trampoline_result(TrampolineResult result)
{

  if (result.panic != std::to_underlying(HostFnErrorCode::NoneOrRecoverable)) {
    return {std::unexpected(static_cast<HostFnErrorCode>(result.panic)),
            result.gas_consumed};
  }

  return {result.result_code, result.gas_consumed};
}

} // namespace

std::pair<InvokeStatus<void>, GasAmount>
Rv32Runtime::invoke_internal(MethodID method_id, uint64_t gas_limit) noexcept
{
  auto machine_noexcept =
      [&]() noexcept -> InvokeStatus<rv32::ThreadedMachine> {
    try {
      rv32::ThreadedMachine machine = contract_->instantiate(gas_limit);
      machine.seed_method_id(method_id);
      return machine;
    } catch (std::exception const &e) {
      error("Unrecoverable: std::exception: {}", e.what());
      return InvokeError{InvokeErrorCode::Unrecoverable};
    }
  }();
  if (!machine_noexcept.has_value()) {
    return {machine_noexcept.error(), 0};
  }

  auto &machine = *machine_noexcept;

  rv32::StepResult result = machine.start();

  while (result == rv32::StepResult::kEcall) {

    auto const &pending_ecall = machine.pending_ecall();
    assert(pending_ecall.has_value());

    ExternHostCallContext host_fn_ctx = syscall_handler_;
    assert(host_fn_ctx != nullptr);

    WasmContext wasm_context = {
        .memory = LinearMemorySlice{.mem = machine.memory().data(),
                                    .sz = machine.memory().size()},
        .gas_remaining = machine.gas_remaining(),
    };

    auto [host_result, gas_consumed] = from_trampoline_result(
        syscall_entry(host_fn_ctx, wasm_context, pending_ecall->a0,
                      pending_ecall->a1, pending_ecall->a2, pending_ecall->a3,
                      pending_ecall->a4, pending_ecall->a5, pending_ecall->a6,
                      pending_ecall->a7));

    if (host_result.has_value()) {
      result = machine.resume_from_ecall(
          rv32::HostResult::continue_with(*host_result, gas_consumed));
      continue;
    }

    assert(gas_consumed <= machine.gas_remaining());
    [[maybe_unused]] bool success = machine.consume_gas(gas_consumed);
    assert(success);

    GasAmount gas_remaining = machine.gas_remaining();
    assert(gas_remaining <= gas_limit);

    GasAmount total_gas_consumed = gas_limit - gas_remaining;

    switch (host_result.error()) {
    case HostFnErrorCode::NoneOrRecoverable:
      error("Unrecoverable: HostFnErrorCode::NoneOrRecoverable is not expected "
            "from TrampolineResult");
      return {InvokeError{InvokeErrorCode::Unrecoverable}, total_gas_consumed};
    case HostFnErrorCode::Unrecoverable:
      return {InvokeError{InvokeErrorCode::Unrecoverable}, total_gas_consumed};
    case HostFnErrorCode::ValidationFailure:
      return {InvokeError{InvokeErrorCode::ValidationFailure},
              total_gas_consumed};
    case HostFnErrorCode::Abandon:
      return {InvokeError{InvokeErrorCode::Abandon}, total_gas_consumed};
    case HostFnErrorCode::Return:
      // Successful return: the value (not error) state of InvokeStatus<void>.
      return {{}, total_gas_consumed};
    case HostFnErrorCode::OutOfGas:
      return {InvokeError{InvokeErrorCode::OutOfGas}, total_gas_consumed};
    case HostFnErrorCode::Exit:
      return {InvokeError{InvokeErrorCode::Deterministic}, total_gas_consumed};
    }
  }

  GasAmount gas_remaining = machine.gas_remaining();
  assert(gas_remaining <= gas_limit);
  GasAmount total_gas_consumed = gas_limit - gas_remaining;

  if (result == rv32::StepResult::kContinue) [[unlikely]] {
    error("Unrecoverable: rv32::StepResult::kContinue when halt expected");
    return {InvokeError{InvokeErrorCode::Unrecoverable}, total_gas_consumed};
  }

  if (result == rv32::StepResult::kHalted) [[unlikely]] {
    error("Unrecoverable: rv32::StepResult::kHalted is not expected from "
          "ThreadedMachine");
    return {InvokeError{InvokeErrorCode::Unrecoverable}, total_gas_consumed};
  }

  assert(result == rv32::StepResult::kTrap);
  assert(machine.trap() != rv32::Trap::kNone);
  assert(machine.trap() != rv32::Trap::kHostTrap);

  if (machine.trap() == rv32::Trap::kOutOfGas) {
    return {InvokeError{InvokeErrorCode::OutOfGas}, total_gas_consumed};
  }

  return {InvokeError{InvokeErrorCode::Deterministic}, total_gas_consumed};
}

InvokeErrorCode
Rv32Context::verify_contract(Contract const &contract) const
{
  if (contract.contract_type() != ContractType::kRv32) {
    return InvokeErrorCode::Deterministic;
  }
  if (!rv32::parse_threaded_artifact(contract.rv32_contract().data)
           .has_value()) {
    return InvokeErrorCode::Deterministic;
  }
  return InvokeErrorCode::None;
}

SystemStatus<std::unique_ptr<Runtime>>
Rv32Context::make_new_runtime(DeployedContract const &deployed_contract,
                              SystemCallHandler *syscall_handler)
{
  DeployedContractHash const &deployed_contract_hash = deployed_contract.hash;
  if (deployed_contract_hash.contract_type != ContractType::kRv32) {
    error("Unrecoverable: Attempt made to access a non-rv32 contract from "
          "Rv32Context: {}",
          deployed_contract_hash.contract_type);
    return SystemError{SystemErrorCode::Unrecoverable};
  }
  assert(syscall_handler != nullptr);

  // Fast path: a snapshot deployment carries a back-pointer to the cached
  // contract entry, which already holds the eagerly-built translation. No
  // resolve_contract round trip, no per-invocation translation.
  if (deployed_contract.entry != nullptr &&
      deployed_contract.entry->translated.has_value()) {
    return std::make_unique<Rv32Runtime>(&*deployed_contract.entry->translated,
                                         syscall_handler);
  }

  // Fallback: either a transaction-local deployment (no cache entry) or a
  // cached entry whose translation is absent. Resolve the code, then borrow a
  // translation owned by the per-transaction handler -- the runtime never owns
  // its Rv32Contract.
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

  const Rv32Contract *translated =
      syscall_handler->get_or_create_local_rv32_translation(
          deployed_contract_hash.hash, *contract.value());
  if (translated == nullptr) [[unlikely]] {
    error("Unrecoverable: failed to translate rv32 contract: {}",
          show_byte_array(deployed_contract_hash.hash));
    return SystemError{SystemErrorCode::Unrecoverable};
  }

  return std::make_unique<Rv32Runtime>(translated, syscall_handler);
}

} // namespace hogd
