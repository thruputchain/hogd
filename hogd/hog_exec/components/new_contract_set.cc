#include "new_contract_set.h"

#include "hogd/base/log.h"

namespace hogd {

// TODO(geoff): gas cost calibration
std::pair<InvokeStatus<NewContractSet>, GasAmount>
NewContractSet::make(NewContractBatch const &contracts,
                     RuntimeContext &runtime_context)
{
  NewContractSet new_contract_set;
  new_contract_set.new_contracts_.reserve(contracts.size());
  new_contract_set.new_contract_preparations_.reserve(contracts.size());

  // Cheap structural gates first, so a malformed attachment set is rejected
  // before any (uncharged, see the TODO above) verification work runs.
  for (auto const &contract : contracts) {
    // Native contracts are registered at genesis, never deployed by a
    // transaction; rv32 is the only deployable type.
    if (contract.contract_type() != ContractType::kRv32) {
      error("Attached contract is not rv32; only rv32 contracts are "
            "deployable");
      return {InvokeError{InvokeErrorCode::ValidationFailure}, 0};
    }
    // Rejected rather than deduplicated: two different transaction encodings
    // would otherwise have identical effect.
    if (!new_contract_set.new_contracts_.emplace(sha256(contract), &contract)
             .second) {
      error("Transaction attaches the same contract more than once");
      return {InvokeError{InvokeErrorCode::ValidationFailure}, 0};
    }
  }

  for (auto const &[hash, contract] : new_contract_set.new_contracts_) {
    PreparedContract prepared_contract;
    auto res = runtime_context.verify_contract(*contract, prepared_contract);
    if (res != InvokeErrorCode::None) {
      // An unverifiable attachment makes the transaction malformed: reject it
      // as a ValidationFailure so it is dropped, never included in a block as
      // a deterministic failure. Only a deterministic verification failure
      // may be remapped -- covering up an Unrecoverable here would hide a
      // genuine node fault.
      CHECK(res == InvokeErrorCode::Deterministic);
      error("Contract verification failed");
      return {InvokeError{InvokeErrorCode::ValidationFailure}, 0};
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
