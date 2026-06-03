#include "hogd/hogexec/precompiles/context.h"

#include <memory>
#include <unordered_map>
#include <utility>

#include "contract.hh"
#include "hogd/base/types.h"
#include "hogd/hogexec/precompiles/precompile_initial_token_claim.h"
#include "hogd/hogexec/precompiles/precompile_native_token.h"
#include "hogd/hogexec/runtime/multiplexed_context.h"
#include "hogd/hogexec/runtime/native_runtime.h"
#include "hogd/hogexec/runtime/runtime.h"
#include "hogd/hogexec/runtime/rv32/rv32_contract.h"
#include "precompiles.hh"

namespace hogd {

std::unique_ptr<RuntimeContext>
make_hogexec_precompiles_context()
{
  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
  contract_map.emplace(
      std::to_underlying(
          HogexecPrecompiledNativeContracts::kNativeContractHogToken),
      make_native_token_contract());
  contract_map.emplace(
      std::to_underlying(
          HogexecPrecompiledNativeContracts::kNativeContractInitialTokenClaim),
      make_native_initial_token_claim_contract());
  return std::make_unique<NativeContext>(std::move(contract_map));
}

std::unique_ptr<RuntimeContext>
make_hogexec_context()
{
  // This thread_local declaration has a meaningful performance impact
  static thread_local std::unique_ptr<RuntimeContext> hogexec_native_context =
      make_hogexec_precompiles_context();

  // RV32 is stateless (it resolves + translates contracts on demand), so a
  // single borrowed instance per thread suffices, like the native context.
  static thread_local Rv32Context hogexec_rv32_context;

  auto hogexec_context = make_hogexec_wasm_context(RuntimeEngine::kWasmi);

  return std::make_unique<MultiplexedContext>(
      std::unordered_map<ContractType, std::pair<RuntimeContext *, bool>>{
          {ContractType::kWasmi, {hogexec_context.release(), true}},
          {ContractType::kNative, {hogexec_native_context.get(), false}},
          {ContractType::kRv32, {&hogexec_rv32_context, false}},
      });
}

} // namespace hogd
