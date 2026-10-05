#include "hogd/hog_exec/runtime/rv32/rv32_contract.h"
#include "hogd/hog_exec/runtime/item/deployed_contract.h"
#include "hogd/hog_exec/runtime/rv32/program.h"

#include <cassert>
#include <exception>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <utility>

#include "contract.hh"
#include "db_conflict.hh"
#include "ffi_enums.hh"
#include "hog_exec_constants.hh"
#include "hogd/base/log.h"
#include "hogd/base/network_config_params.h"
#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h"
#include "hogd/hog_exec/common/status.h"
#include "hogd/hog_exec/runtime/item/storage_snapshot_cache.h"
#include "hogd/hog_exec/runtime/prepared_contract.h"
#include "hogd/hog_exec/runtime/runtime.h"
#include "hogd/hog_exec/runtime/runtime_exports.h"
#include "hogd/hog_exec/runtime/rv32/artifact.h"
#include "hogd/hog_exec/runtime/rv32/host.h"
#include "hogd/hog_exec/runtime/rv32/threaded_machine.h"
#include "hogd/hog_exec/runtime/syscall_entry.h"
#include "hogd/hog_exec/runtime/syscall_handler.h"
#include "types.hh"
#include "x23/async.h"

namespace hogd {

// The VM's defaults restate the protocol's.
static_assert(rv32::kDefaultMaxCodeBytes ==
              static_cast<uint32_t>(kDefaultMaxRv32CodeBytes));
static_assert(rv32::kDefaultMaxMemoryBytes ==
              static_cast<uint32_t>(kDefaultMaxRv32MemoryBytes));

std::expected<Rv32Contract, rv32::ArtifactError>
Rv32Contract::create(ContractBlob const &contract_blob, rv32::Rv32Limits limits)
{
  auto artifact = rv32::parse_threaded_artifact(contract_blob.data, limits);
  if (!artifact.has_value()) {
    return std::unexpected(artifact.error());
  }

  auto pre_instantiation = rv32::ThreadedMachinePreInstantiation::create(
      artifact->code, artifact->initial_memory, artifact->initial_memory_start,
      artifact->memory_bytes, false);
  if (!pre_instantiation.has_value()) {
    return std::unexpected(rv32::ArtifactError::kInvalidProgram);
  }

  return Rv32Contract(std::move(*pre_instantiation));
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

  // Charge for the memory-image restore up front; a child that cannot afford
  // it faults out of gas before the copy, so the bill scales with image size
  // and a zero-gas child cannot copy a large image for free (#4354).
  const uint64_t memory_restore_gas = contract_->memory_restore_gas();
  if (memory_restore_gas > gas_limit) {
    return result_t{InvokeError{InvokeErrorCode::OutOfGas}, gas_limit};
  }

  auto machine_noexcept =
      [&]() noexcept -> InvokeStatus<rv32::ThreadedMachine> {
    try {
      rv32::ThreadedMachine machine = contract_->instantiate(gas_limit);
      // Debit the restore cost checked above (guaranteed to fit); the rest of
      // the budget meters the guest's instructions.
      [[maybe_unused]] const bool afforded =
          machine.consume_gas(memory_restore_gas);
      assert(afforded);
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

    // Syscall outputs write directly into live guest memory. Handlers requiring
    // atomic output must stage their writes before touching the output spans.
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
      // from_trampoline_result maps panic == 0 to success, so this error is
      // unreachable under the trampoline contract.
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

namespace {

// The wire reason for an artifact Rv32Contract::create refused. Coarser than
// ArtifactError: the wire names what an author can act on, not every check.
ContractRejectCode
to_contract_reject_code(rv32::ArtifactError error)
{
  using enum rv32::ArtifactError;
  switch (error) {
  case kTooSmall:
  case kBadMagic:
  case kBadHeaderSize:
  case kInvalidSectionLayout:
    return ContractRejectCode::ContractRejectMalformedArtifact;
  case kUnsupportedFormatVersion:
    return ContractRejectCode::ContractRejectUnsupportedFormat;
  case kUnsupportedVmProfile:
  case kUnsupportedFlags:
  case kUnsupportedEntryAbi:
  case kUnsupportedSyscallAbi:
    return ContractRejectCode::ContractRejectUnsupportedAbi;
  case kGasScheduleVersionMismatch:
  case kGasScheduleDigestMismatch:
    return ContractRejectCode::ContractRejectGasScheduleMismatch;
  case kMemorySizeInvalid:
    return ContractRejectCode::ContractRejectMemoryTooLarge;
  case kCodeSizeInvalid:
    return ContractRejectCode::ContractRejectCodeSizeInvalid;
  case kInitialMemoryTooLarge:
    return ContractRejectCode::ContractRejectInitialMemoryInvalid;
  case kCodeDigestMismatch:
  case kInitialMemoryDigestMismatch:
    return ContractRejectCode::ContractRejectDigestMismatch;
  case kInvalidProgram:
    return ContractRejectCode::ContractRejectInvalidProgram;
  }
  // Every enumerator returns above; an out-of-range value is memory corruption.
  CHECK(false);
  return ContractRejectCode::ContractRejectMalformedArtifact;
}

} // namespace

InvokeErrorCode
Rv32Context::verify_contract(Contract const &contract,
                             NetworkConfigParams const &params,
                             PreparedContract &out,
                             std::optional<ContractRejectCode> &rejection) const
{
  if (contract.contract_type() != ContractType::kRv32) {
    rejection = ContractRejectCode::ContractRejectNotDeployable;
    return InvokeErrorCode::Deterministic;
  }
  // Verify by translating (parsing alone misses decode failures) under the
  // chain's bounds, and hand the translation back for reuse (#3536).
  auto translated = Rv32Contract::create(
      contract.rv32_contract(),
      rv32::Rv32Limits{.max_code_bytes = params.max_rv32_code_bytes,
                       .max_memory_bytes = params.max_rv32_memory_bytes});
  if (!translated.has_value()) {
    rejection = to_contract_reject_code(translated.error());
    return InvokeErrorCode::Deterministic;
  }
  out.rv32.emplace(std::move(*translated));
  return InvokeErrorCode::None;
}

x23::Async<SystemStatus<std::unique_ptr<Runtime>>>
Rv32Context::make_new_runtime(DeployedContract const &deployed_contract,
                              SystemCallHandler *syscall_handler) const
{
  DeployedContractHash const &deployed_contract_hash = deployed_contract.hash;
  if (deployed_contract_hash.contract_type != ContractType::kRv32) {
    error("Unrecoverable: Attempt made to access a non-rv32 contract from "
          "Rv32Context: {}",
          deployed_contract_hash.contract_type);
    co_return SystemError{SystemErrorCode::Unrecoverable};
  }
  assert(syscall_handler != nullptr);

  // Snapshot deployment: the cache translated the entry eagerly and never
  // holds an rv32 entry without a translation, so this check is a guard.
  if (deployed_contract.entry != nullptr) {
    if (!deployed_contract.entry->prepared.rv32.has_value()) [[unlikely]] {
      error("Unrecoverable: rv32 contract is not translatable: {}",
            show_byte_array(deployed_contract_hash.hash));
      co_return SystemError{SystemErrorCode::Unrecoverable};
    }
    co_return std::make_unique<Rv32Runtime>(
        &*deployed_contract.entry->prepared.rv32, syscall_handler);
  }

  // Transaction-local deployment: the address is not in the block-scoped
  // deployment cache, so there is no back-pointer. Check the code against the
  // hash metadata, then borrow the image the handler resolves for it -- from
  // initial validation for an attachment, from the snapshot cache for an
  // already-committed contract. Nothing is translated here; the runtime never
  // owns its Rv32Contract.
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

  auto prepared_res = co_await syscall_handler->resolve_prepared_contract(
      deployed_contract_hash.hash);
  if (!prepared_res.has_value()) [[unlikely]] {
    co_return prepared_res.error();
  }
  const PreparedContract *prepared = *prepared_res;
  if (prepared == nullptr) [[unlikely]] {
    error("Unrecoverable: Contract lookup failed: {}",
          show_byte_array(deployed_contract_hash.hash));
    co_return SystemError{SystemErrorCode::Unrecoverable};
  }
  if (!prepared->rv32.has_value()) [[unlikely]] {
    error("Unrecoverable: rv32 contract is not translatable: {}",
          show_byte_array(deployed_contract_hash.hash));
    co_return SystemError{SystemErrorCode::Unrecoverable};
  }

  co_return std::make_unique<Rv32Runtime>(&*prepared->rv32, syscall_handler);
}

} // namespace hogd
