#include "hogd/hog_exec/runtime/rv32/rv32_contract.h"
#include "hogd/hog_exec/runtime/item/deployed_contract.h"

#include <cassert>
#include <exception>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <utility>

#include "contract.hh"
#include "ffi_enums.hh"
#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h"
#include "hogd/hog_exec/common/status.h"
#include "hogd/hog_exec/runtime/item/storage_snapshot_cache.h"
#include "hogd/hog_exec/runtime/runtime.h"
#include "hogd/hog_exec/runtime/runtime_exports.h"
#include "hogd/hog_exec/runtime/rv32/artifact.h"
#include "hogd/hog_exec/runtime/rv32/host.h"
#include "hogd/hog_exec/runtime/rv32/threaded_machine.h"
#include "hogd/hog_exec/runtime/syscall_entry.h"
#include "hogd/hog_exec/runtime/syscall_handler.h"
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
      artifact->code, artifact->initial_memory, artifact->initial_memory_start,
      artifact->memory_bytes, false);
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

x23::MaybeAsync<std::pair<InvokeStatus<void>, GasAmount>>
Rv32Runtime::invoke_internal(MethodID method_id, uint64_t gas_limit) noexcept
{
  using result_t = std::pair<InvokeStatus<void>, GasAmount>;
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
    return result_t{machine_noexcept.error(), 0};
  }

  // Creating the loop coroutine allocates its frame and can throw; this
  // function is noexcept, so translate that the same way an instantiation
  // failure is translated above.
  try {
    return run_ecall_loop(std::move(*machine_noexcept), gas_limit);
  } catch (std::exception const &e) {
    error("Unrecoverable: could not start rv32 ecall loop: {}", e.what());
    return result_t{InvokeError{InvokeErrorCode::Unrecoverable}, 0};
  } catch (...) {
    report_dropped_log("non-std exception starting rv32 ecall loop");
    return result_t{InvokeError{InvokeErrorCode::Unrecoverable}, 0};
  }
}

// The machine is moved into this coroutine's frame: syscalls suspend the loop,
// so it cannot live on the caller's stack.
x23::Async<std::pair<InvokeStatus<void>, GasAmount>>
Rv32Runtime::run_ecall_loop(rv32::ThreadedMachine machine, uint64_t gas_limit)
{
  using result_t = std::pair<InvokeStatus<void>, GasAmount>;
  rv32::StepResult result = machine.start();

  while (result == rv32::StepResult::kEcall) {

    auto const &pending_ecall = machine.pending_ecall();
    assert(pending_ecall.has_value());

    ExternHostCallContext host_fn_ctx = syscall_handler_;
    assert(host_fn_ctx != nullptr);

    // Production RV32 intentionally exposes live guest memory to syscall_entry:
    // generated syscall output spans write directly into the ThreadedMachine's
    // memory for speed. Syscall handlers that need all-or-nothing output
    // semantics must stage internally before touching those spans.
    ExecutionContext context = {
        .memory = std::as_writable_bytes(machine.memory()),
        .gas_remaining = machine.gas_remaining(),
    };

    auto [host_result, gas_consumed] =
        co_await syscall_entry(host_fn_ctx, context, pending_ecall->a0,
                               pending_ecall->a1, pending_ecall->a2,
                               pending_ecall->a3, pending_ecall->a4,
                               pending_ecall->a5, pending_ecall->a6,
                               pending_ecall->a7);

    if (host_result.has_value()) {
      result = machine.resume_from_ecall(
          rv32::HostResult::continue_with(std::to_underlying(*host_result),
                                          gas_consumed));
      continue;
    }

    assert(gas_consumed <= machine.gas_remaining());
    [[maybe_unused]] bool success = machine.consume_gas(gas_consumed);
    assert(success);

    GasAmount gas_remaining = machine.gas_remaining();
    assert(gas_remaining <= gas_limit);

    GasAmount total_gas_consumed = gas_limit - gas_remaining;

    switch (host_result.error().code()) {
    case HostFnErrorCode::NoneOrRecoverable:
      // from_trampoline_result routes panic == 0 to the continue branch, so
      // this arm is reachable only if that contract changes; abort loudly in
      // debug test runs rather than silently reporting Unrecoverable.
      assert(false);
      error("Unrecoverable: HostFnErrorCode::NoneOrRecoverable is not expected "
            "from TrampolineResult");
      co_return result_t{InvokeError{InvokeErrorCode::Unrecoverable},
                         total_gas_consumed};
    case HostFnErrorCode::Unrecoverable:
      co_return result_t{InvokeError{InvokeErrorCode::Unrecoverable},
                         total_gas_consumed};
    case HostFnErrorCode::ValidationFailure:
      co_return result_t{InvokeError{InvokeErrorCode::ValidationFailure},
                         total_gas_consumed};
    case HostFnErrorCode::Abandon:
      co_return result_t{InvokeError{InvokeErrorCode::Abandon},
                         total_gas_consumed};
    case HostFnErrorCode::Return:
      // Successful return: the value (not error) state of InvokeStatus<void>.
      co_return result_t{{}, total_gas_consumed};
    case HostFnErrorCode::OutOfGas:
      co_return result_t{InvokeError{InvokeErrorCode::OutOfGas},
                         total_gas_consumed};
    case HostFnErrorCode::Exit:
      co_return result_t{InvokeError{InvokeErrorCode::Deterministic},
                         total_gas_consumed};
    }
  }

  GasAmount gas_remaining = machine.gas_remaining();
  assert(gas_remaining <= gas_limit);
  GasAmount total_gas_consumed = gas_limit - gas_remaining;

  if (result == rv32::StepResult::kContinue) [[unlikely]] {
    error("Unrecoverable: rv32::StepResult::kContinue when halt expected");
    co_return result_t{InvokeError{InvokeErrorCode::Unrecoverable},
                       total_gas_consumed};
  }

  if (result == rv32::StepResult::kHalted) [[unlikely]] {
    error("Unrecoverable: rv32::StepResult::kHalted is not expected from "
          "ThreadedMachine");
    co_return result_t{InvokeError{InvokeErrorCode::Unrecoverable},
                       total_gas_consumed};
  }

  assert(result == rv32::StepResult::kTrap);
  assert(machine.trap() != rv32::Trap::kNone);
  assert(machine.trap() != rv32::Trap::kHostTrap);

  if (machine.trap() == rv32::Trap::kOutOfGas) {
    co_return result_t{InvokeError{InvokeErrorCode::OutOfGas},
                       total_gas_consumed};
  }

  co_return result_t{InvokeError{InvokeErrorCode::Deterministic},
                     total_gas_consumed};
}

InvokeErrorCode
Rv32Context::verify_contract(Contract const &contract) const
{
  if (contract.contract_type() != ContractType::kRv32) {
    return InvokeErrorCode::Deterministic;
  }
  // Verify full translatability, not just artifact parsing: parse_threaded_-
  // artifact only runs the structural artifact checks, whereas Rv32Contract::-
  // create additionally builds the threaded program (instruction decode) and
  // the pre-instantiation image, which can reject inputs that parse cleanly.
  // Rejecting non-translatable contracts at creation time is what makes the
  // "not translatable" path in make_new_runtime unreachable for a deployed
  // contract (so its Unrecoverable error is a true should-never-happen).
  if (!Rv32Contract::create(contract.rv32_contract()).has_value()) {
    return InvokeErrorCode::Deterministic;
  }
  return InvokeErrorCode::None;
}

x23::Async<SystemStatus<std::unique_ptr<Runtime>>>
Rv32Context::make_new_runtime(DeployedContract const &deployed_contract,
                              SystemCallHandler *syscall_handler)
{
  DeployedContractHash const &deployed_contract_hash = deployed_contract.hash;
  if (deployed_contract_hash.contract_type != ContractType::kRv32) {
    error("Unrecoverable: Attempt made to access a non-rv32 contract from "
          "Rv32Context: {}",
          deployed_contract_hash.contract_type);
    co_return SystemError{SystemErrorCode::Unrecoverable};
  }
  assert(syscall_handler != nullptr);

  // Snapshot deployment: the back-pointer entry was translated eagerly when the
  // contract entered the cache. A present translation is the fast path (no
  // resolve_contract round trip, no per-invocation translation); a null
  // translation is the cached result for a non-translatable artifact, so fail
  // here rather than re-resolving and re-translating (which would just fail
  // again).
  if (deployed_contract.entry != nullptr) {
    if (!deployed_contract.entry->translated.has_value()) [[unlikely]] {
      error("Unrecoverable: rv32 contract is not translatable: {}",
            show_byte_array(deployed_contract_hash.hash));
      co_return SystemError{SystemErrorCode::Unrecoverable};
    }
    co_return std::make_unique<Rv32Runtime>(
        &*deployed_contract.entry->translated, syscall_handler);
  }

  // Transaction-local deployment: not in the block-scoped snapshot cache.
  // Resolve the code, then borrow a translation owned by the per-transaction
  // handler (which memoizes failures too, so we never retranslate) -- the
  // runtime never owns its Rv32Contract.
  auto contract =
      co_await syscall_handler->resolve_contract(deployed_contract_hash.hash);
  if (!contract.has_value()) [[unlikely]] {
    co_return contract.error();
  }
  if (contract.value() == nullptr) [[unlikely]] {
    error("Unrecoverable: Contract lookup failed: {}",
          show_byte_array(deployed_contract_hash.hash));
    co_return SystemError{SystemErrorCode::Unrecoverable};
  }
  if (contract.value()->contract_type() != deployed_contract_hash.contract_type)
      [[unlikely]] {
    error("Unrecoverable: Mismatch in contract type: {} != {}",
          contract.value()->contract_type(),
          deployed_contract_hash.contract_type);
    co_return SystemError{SystemErrorCode::Unrecoverable};
  }

  const Rv32Contract *translated =
      syscall_handler->get_or_create_local_rv32_translation(
          deployed_contract_hash.hash, *contract.value());
  if (translated == nullptr) [[unlikely]] {
    error("Unrecoverable: failed to translate rv32 contract: {}",
          show_byte_array(deployed_contract_hash.hash));
    co_return SystemError{SystemErrorCode::Unrecoverable};
  }

  co_return std::make_unique<Rv32Runtime>(translated, syscall_handler);
}

} // namespace hogd
