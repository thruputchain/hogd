#include "loadgen_server.h"

#include <memory>
#include <utility>

#include "hogd/base/types.h"
#include "hogd/loadgen/erc20_basic_auth_experiment.h"
#include "hogd/loadgen/erc20_experiment.h"
#include "hogd/loadgen/experiment_state.h"
#include "hogd/loadgen/status.h"
#include "loadgen.hh"
#include "types.hh"
#include "x23/async.h"

namespace hogd {

x23::Async<LoadgenResult>
LoadgenRpc::generate_experiment(ExperimentConfig config)
{
  switch (config.experiment_type()) {
  case ExperimentType::kExperimentTypeERC20Payments:
  {
    auto state = std::make_shared<ERC20ExperimentState>(
        config.erc20_payments(), false, num_virtual_addresses_,
        filler_instances_, filler_items_, filler_item_bytes_, filler_period_);
    auto res = co_await state->generate_experiment();
    if (!res) {
      co_return LoadgenResult(res.error().code());
    }
    LoadgenResult out(LoadgenRPCError::kLoadgenRPCErrorOK);
    out.success() = std::move(res.value());
    uint64_t context_id = context_id_counter_++;
    out.success().context_id = context_id;
    experiment_states_.take()->emplace(context_id, state);
    co_return out;
  }
  case ExperimentType::kExperimentTypeERC20BasicAuthPayments:
  {
    auto state = std::make_shared<ERC20BasicAuthExperimentState>(
        config.erc20_payments(), num_virtual_addresses_, filler_instances_,
        filler_items_, filler_item_bytes_, filler_period_, filler_keyspace_);
    auto res = co_await state->generate_experiment();
    if (!res) {
      co_return LoadgenResult(res.error().code());
    }

    LoadgenResult out(LoadgenRPCError::kLoadgenRPCErrorOK);
    out.success() = std::move(res.value());
    uint64_t context_id = context_id_counter_++;
    out.success().context_id = context_id;
    experiment_states_.take()->emplace(context_id, state);
    co_return out;
  }
  default:
    co_return LoadgenResult(LoadgenRPCError::kLoadgenRPCErrorInvalidConfig);
  }
}

x23::Async<LoadgenExtensionResult>
LoadgenRpc::extend_experiment(LoadgenContextID context_id, uint64_t num_txs,
                              BlockHeight expiry_height)
{
  // Copy the shared_ptr out under the lock so we don't hold std::mutex
  // across the co_await below: a coroutine may resume on a different
  // thread than the one that locked it, and unlocking std::mutex from
  // the wrong thread is undefined behavior.
  std::shared_ptr<ExperimentState> state;
  {
    auto st = experiment_states_.take();
    auto it = st->find(context_id);
    if (it == st->end()) {
      co_return LoadgenExtensionResult(
          LoadgenRPCError::kLoadgenRPCErrorInvalidContext);
    }
    state = it->second;
  }
  LoadgenExtensionResult out(LoadgenRPCError::kLoadgenRPCErrorOK);
  LoadgenStatus<LoadgenExtension> r =
      co_await state->extend_experiment(num_txs, expiry_height);
  if (!r) {
    co_return LoadgenExtensionResult(r.error().code());
  }
  out.success() = std::move(r.value());
  co_return out;
}

} // namespace hogd
