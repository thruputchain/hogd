#include "new_contract_set.h"
#include "hogd/base/network_config_params.h"

#include <optional>
#include <utility>

#include "contract.hh"
#include "db_conflict.hh"
#include "executor_to_db_rpc.hh"
#include "ffi_enums.hh"
#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "hogd/crypto/crypto.h"
#include "hogd/hog_exec/common/status.h"
#include "hogd/hog_exec/runtime/prepared_contract.h"
#include "hogd/hog_exec/runtime/runtime.h"
#include "types.hh"

namespace hogd {

// TODO(geoff): gas cost calibration
std::pair<InvokeStatus<NewContractSet>, GasAmount>
NewContractSet::make(NewContractBatch const &contracts,
                     RuntimeContext &runtime_context,
                     NetworkConfigParams const &params,
                     std::optional<ContractRejectDiagnostic> &rejection)
{
  using result_t = std::pair<InvokeStatus<NewContractSet>, GasAmount>;
  auto reject = [&rejection](size_t index, ContractRejectCode code) {
    rejection =
        ContractRejectDiagnostic{.contract_index = static_cast<uint32_t>(index),
                                 .code = code};
    return result_t{InvokeError{InvokeErrorCode::ValidationFailure}, 0};
  };

  NewContractSet new_contract_set;
  new_contract_set.new_contracts_.reserve(contracts.size());
  new_contract_set.new_contract_preparations_.reserve(contracts.size());

  // Cheap structural gates first, so a malformed attachment set is rejected
  // before any (uncharged, see the TODO above) verification work runs.
  for (size_t i = 0; i < contracts.size(); ++i) {
    auto const &contract = contracts[i];
    // Native contracts are registered at genesis, never deployed by a
    // transaction; rv32 is the only deployable type.
    if (contract.contract_type() != ContractType::kRv32) {
      return reject(i, ContractRejectCode::ContractRejectNotDeployable);
    }
    // Rejected rather than deduplicated: two different transaction encodings
    // would otherwise have identical effect.
    if (!new_contract_set.new_contracts_.emplace(sha256(contract), &contract)
             .second) {
      return reject(i, ContractRejectCode::ContractRejectDuplicate);
    }
  }

  for (auto const &[hash, contract] : new_contract_set.new_contracts_) {
    PreparedContract prepared_contract;
    std::optional<ContractRejectCode> reason;
    auto res = runtime_context.verify_contract(*contract, params,
                                               prepared_contract, reason);
    // An invalid attachment makes the transaction malformed: reject it as a
    // ValidationFailure so it is dropped, never included in a block as a
    // deterministic failure. The map holds pointers into [contracts], so the
    // offset is the attachment's index.
    if (res == InvokeErrorCode::Deterministic) {
      if (reason.has_value()) {
        return reject(static_cast<size_t>(contract - contracts.data()),
                      *reason);
      }
      return {InvokeError{InvokeErrorCode::ValidationFailure}, 0};
    }
    // Anything else is a node fault, not the contract's: pass it through.
    if (res != InvokeErrorCode::None) {
      return {InvokeError{res}, 0};
    }
    // rv32 is the only type that reaches here, and for rv32 verifying an
    // attachment *is* translating it, so a successful verification always
    // yields an image. Keep it: this is what stops the first
    // same-transaction invocation from lowering the identical bytes again,
    // both passes outside transaction gas (#3536).
    CHECK(prepared_contract.rv32.has_value());
    new_contract_set.new_contract_preparations_.emplace(hash,
                                                        std::move(
                                                            prepared_contract));
  }
  return {std::move(new_contract_set), 0};
}

} // namespace hogd
