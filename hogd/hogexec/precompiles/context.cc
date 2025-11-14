#include "hogd/hogexec/precompiles/context.h"

#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "contract.hh"
#include "hogd/base/types.h"
#include "hogd/hogexec/precompiles/precompile_native_token.h"
#include "hogd/hogexec/runtime/multiplexed_context.h"
#include "hogd/hogexec/runtime/native_runtime.h"
#include "hogd/hogexec/runtime/runtime.h"
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
  return std::make_unique<NativeContext>(std::move(contract_map));
}

std::unique_ptr<RuntimeContext>
make_hogexec_context()
{
  static std::unique_ptr<RuntimeContext> hogexec_native_context =
      make_hogexec_precompiles_context();

  auto hogexec_context = make_hogexec_wasm_context(RuntimeEngine::kWasmi);
  auto* hogexec_context_ptr = hogexec_context.get();
  std::vector<std::unique_ptr<RuntimeContext>> owned_contexts;
  owned_contexts.reserve(1);
  owned_contexts.emplace_back(std::move(hogexec_context));

  return std::make_unique<MultiplexedContext>(
      std::unordered_map<ContractType, RuntimeContext *>{
          {ContractType::kWasmi, hogexec_context_ptr},
          {ContractType::kNative, hogexec_native_context.get()},
      },
      std::move(owned_contexts));
}

} // namespace hogd
