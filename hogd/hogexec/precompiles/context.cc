#include "hogd/hogexec/precompiles/context.h"

#include <memory>
#include <unordered_map>
#include <utility>

#include "contract.hh"
#include "hogd/base/types.h"
#include "hogd/hogexec/precompiles/precompile_native_token.h"
#include "hogd/hogexec/runtime/multiplexed_context.h"
#include "hogd/hogexec/runtime/native_runtime.h"
#include "hogd/hogexec/runtime/runtime.h"
#include "precompiles.hh"

namespace hogd {

std::shared_ptr<RuntimeContext>
make_hogexec_precompiles_context()
{
  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
  contract_map.emplace(
      std::to_underlying(
          HogexecPrecompiledNativeContracts::kNativeContractHogToken),
      make_native_token_contract());
  return std::make_shared<NativeContext>(std::move(contract_map));
}

std::shared_ptr<RuntimeContext>
make_hogexec_context()
{
  return std::make_shared<MultiplexedContext>(
      std::unordered_map<ContractType, std::shared_ptr<RuntimeContext>>{
          {ContractType::kWasmi,
           make_hogexec_wasm_context(RuntimeEngine::kWasmi)},
          {ContractType::kNative, make_hogexec_precompiles_context()},
      });
}

} // namespace hogd
