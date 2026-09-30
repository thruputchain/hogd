#include "new_contract_set.h"
#include "hogd/base/network_config_params.h"

#include <expected>
#include <utility>
#include <vector>

#include "contract.hh"
#include "db_conflict.hh"
#include "executor_to_db_rpc.hh"
#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "hogd/crypto/crypto.h"
#include "hogd/hog_exec/runtime/prepared_contract.h"
#include "hogd/hog_exec/runtime/runtime.h"
#include "types.hh"

namespace hogd {

// TODO(geoff): gas cost calibration
std::pair<std::expected<NewContractSet, ContractRejectDiagnostic>, GasAmount>
NewContractSet::make(NewContractBatch const &contracts,
                     RuntimeContext &runtime_context,
                     NetworkConfigParams const &params)
{
  using result_t =
      std::pair<std::expected<NewContractSet, ContractRejectDiagnostic>,
                GasAmount>;
  auto reject = [](size_t index, ContractRejectCode code) {
    return result_t{std::unexpected(ContractRejectDiagnostic{
                        .contract_index = static_cast<uint32_t>(index),
                        .code = code}),
                    0};
  };

  NewContractSet new_contract_set;
  new_contract_set.new_contracts_.reserve(contracts.size());
  new_contract_set.new_contract_preparations_.reserve(contracts.size());
  std::vector<SHA256Hash> hashes;
  hashes.reserve(contracts.size());

  // Cheap structural gates first, so a malformed attachment set is rejected
  // before any (uncharged, see the TODO above) verification work runs.
  // Both passes walk transaction order: the reported index is the first one.
  for (size_t i = 0; i < contracts.size(); ++i) {
    auto const &contract = contracts[i];
    // Native contracts are registered at genesis, never deployed by a
    // transaction; rv32 is the only deployable type.
    if (contract.contract_type() != ContractType::kRv32) {
      return reject(i, ContractRejectCode::ContractRejectNotDeployable);
    }
    // Rejected rather than deduplicated: two different transaction encodings
    // would otherwise have identical effect.
    auto hash = sha256(contract);
    if (!new_contract_set.new_contracts_.emplace(hash, &contract).second) {
      return reject(i, ContractRejectCode::ContractRejectDuplicate);
    }
    hashes.push_back(hash);
  }

  for (size_t i = 0; i < contracts.size(); ++i) {
    PreparedContract prepared_contract;
    // An unverifiable attachment makes the transaction malformed: the caller
    // rejects it as a ValidationFailure so it is dropped, never included in a
    // block as a deterministic failure.
    if (auto res = runtime_context.verify_contract(contracts[i], params,
                                                   prepared_contract);
        !res.has_value()) {
      return reject(i, res.error());
    }
    // rv32 is the only type that reaches here, and for rv32 verifying an
    // attachment *is* translating it, so a successful verification always
    // yields an image. Keep it: this is what stops the first
    // same-transaction invocation from lowering the identical bytes again,
    // both passes outside transaction gas (#3536).
    CHECK(prepared_contract.rv32.has_value());
    new_contract_set.new_contract_preparations_.emplace(hashes[i],
                                                        std::move(
                                                            prepared_contract));
  }
  return {std::move(new_contract_set), 0};
}

} // namespace hogd
