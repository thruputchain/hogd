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
  // Both sub-contexts are shared, immutable and process-lifetime. Caching them
  // is what matters for performance -- rebuilding the precompile table on every
  // call is the cost being avoided -- and neither has to be per-thread to get
  // that: NativeContext holds only a const contract map (every precompile
  // function is a captureless lambda) and Rv32Context has no members at all, so
  // the RuntimeContext const * borrows below are read-only. That is enforced,
  // not just asserted: both entry points are const (see runtime.h), so a future
  // sub-context that tries to memoize through one of these will fail to
  // compile rather than silently race.
  //
  // They were `thread_local`, which is what made issue #2682 a use-after-free:
  // the borrowed pointee died with the creating thread while a heap
  // MultiplexedContext (e.g. one escaping through the test_ffi C handle) went
  // on borrowing it. Function-local statics are constructed on the first call
  // here, so they always complete construction before any borrower does, and
  // reverse-order destruction therefore tears every borrower down first.
  static const std::unique_ptr<const RuntimeContext> hog_exec_native_context =
      make_hog_exec_precompiles_context();
  static const Rv32Context hog_exec_rv32_context{};

  return std::make_unique<MultiplexedContext>(
      std::unordered_map<ContractType, std::pair<RuntimeContext const *, bool>>{
          {ContractType::kNative, {hog_exec_native_context.get(), false}},
          {ContractType::kRv32, {&hog_exec_rv32_context, false}},
      });
}

} // namespace hogd
