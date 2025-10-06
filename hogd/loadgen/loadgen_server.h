#pragma once

#include <atomic>
#include <memory>
#include <sys/socket.h>
#include <thread>
#include <type_traits>
#include <unordered_map>

#include "hogd/base/rpc_server.h"
#include "hogd/base/status.h"
#include "hogd/base/types.h"
#include "hogd/loadgen/experiment_state.h"
#include "loadgen.hh"
#include "x23/cleanup.h"
#include "x23/mvar.h"

namespace hogd {

class LoadgenRpc {
public:
  using rpc_service = LoadgenInterfaceV1;

  LoadgenResult generate_experiment(ExperimentConfig config);
  LoadgenExtensionResult extend_experiment(LoadgenContextID context_id,
                                           uint64_t num_txs);

private:
  x23::MVar<
      std::unordered_map<LoadgenContextID, std::shared_ptr<ExperimentState>>>
      experiment_states_;
  std::atomic<uint64_t> context_id_counter_;
};

static_assert(is_LoadgenInterfaceV1<LoadgenRpc *, std::type_identity_t>);

class LoadgenServer : public RpcServer<LoadgenRpc> {};

} // namespace hogd
