#pragma once

#include <atomic>
#include <memory>
#include <type_traits>
#include <unordered_map>

#include "hogd/base/rpc_server.h"
#include "hogd/base/types.h"
#include "hogd/loadgen/experiment_state.h"
#include "loadgen.hh"
#include "types.hh"
#include "x23/async.h"
#include "x23/mvar.h"

namespace hogd {

class LoadgenRpc {
public:
  using rpc_service = LoadgenInterfaceV1;

  // We set the RPC max message size to a large value because the loadgen
  // transfers a lot of data via individual RPC calls.
  static constexpr size_t kRpcMaxMessageSize = 0x7fffffffff;

  LoadgenResult generate_experiment(ExperimentConfig config);

  x23::Async<LoadgenExtensionResult>
  extend_experiment(LoadgenContextID context_id, uint64_t num_txs,
                    BlockHeight expiry_height);

  void shut_down()
  {
    // TODO(jhumphri): Implement.
  }

private:
  x23::MVar<
      std::unordered_map<LoadgenContextID, std::shared_ptr<ExperimentState>>>
      experiment_states_;
  std::atomic<uint64_t> context_id_counter_;
};

static_assert(is_LoadgenInterfaceV1<LoadgenRpc *, std::type_identity_t>);

} // namespace hogd
