#include "loadgen_server.h"

#include <memory>
#include <utility>

#include "hogd/base/types.h"
#include "hogd/loadgen/erc20_experiment.h"
#include "loadgen.hh"

namespace hogd {

LoadgenResult
LoadgenRpc::generate_experiment(ExperimentConfig config)
{
  switch (config.experiment_type()) {
  case ExperimentType::kExperimentTypeERC20Payments:
  {
    auto state =
        std::make_shared<ERC20ExperimentState>(config.erc20_payments());
    auto res = state->generate_experiment();
    if (!res) {
      return LoadgenResult(res.error().code());
    }
    LoadgenResult out(LoadgenRPCError::kLoadgenRPCErrorOK);
    out.success() = std::move(res.value());
    uint64_t context_id = context_id_counter_++;
    out.success().context_id = context_id;
    experiment_states_.take()->emplace(context_id, state);
    return out;
  }
  default:
    return LoadgenResult(LoadgenRPCError::kLoadgenRPCErrorInvalidConfig);
  }
}

LoadgenExtensionResult
LoadgenRpc::extend_experiment(LoadgenContextID context_id, uint64_t num_txs)
{
  auto st = experiment_states_.take();
  auto state = st->find(context_id);
  if (state == st->end()) {
    return LoadgenExtensionResult(
        LoadgenRPCError::kLoadgenRPCErrorInvalidContext);
  }
  LoadgenExtensionResult out(LoadgenRPCError::kLoadgenRPCErrorOK);
  auto r = state->second->extend_experiment(num_txs);
  if (!r) {
    return LoadgenExtensionResult(r.error().code());
  }
  out.success() = std::move(r.value());
  return out;
}

} // namespace hogd
