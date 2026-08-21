#include "hogd/hog_exec/precompiles/context.h"

#include <memory>
#include <unordered_map>
#include <utility>

#include "contract.hh"
#include "hogd/base/types.h"
#include "hogd/hog_exec/precompiles/precompile_default_wallet.h"
#include "hogd/hog_exec/precompiles/precompile_native_token.h"
#include "hogd/hog_exec/runtime/multiplexed_context.h"
#include "hogd/hog_exec/runtime/native_runtime.h"
#include "hogd/hog_exec/runtime/runtime.h"
#include "hogd/hog_exec/runtime/rv32/rv32_contract.h"
#include "precompiles.hh"

namespace hogd {

std::unique_ptr<RuntimeContext>
make_hog_exec_precompiles_context()
{
  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
  contract_map.emplace(
      std::to_underlying(
          HogexecPrecompiledNativeContracts::kNativeContractHogToken),
      make_native_token_contract());
  contract_map.emplace(
      std::to_underlying(
          HogexecPrecompiledNativeContracts::kNativeContractDefaultWallet),
      make_native_default_wallet_contract());
  return std::make_unique<NativeContext>(std::move(contract_map));
}

std::unique_ptr<RuntimeContext>
make_hog_exec_context()
{
  // This thread_local declaration has a meaningful performance impact
  static thread_local std::unique_ptr<RuntimeContext> hog_exec_native_context =
      make_hog_exec_precompiles_context();

  // RV32 is stateless (it resolves + translates contracts on demand), so a
  // single borrowed instance per thread suffices, like the native context.
  static thread_local Rv32Context hog_exec_rv32_context;

  return std::make_unique<MultiplexedContext>(
      std::unordered_map<ContractType, std::pair<RuntimeContext *, bool>>{
          {ContractType::kNative, {hog_exec_native_context.get(), false}},
          {ContractType::kRv32, {&hog_exec_rv32_context, false}},
      });
}

} // namespace hogd
