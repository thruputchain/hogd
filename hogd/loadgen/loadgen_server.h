#pragma once

#include <atomic>
#include <memory>
#include <sys/socket.h>
#include <thread>
#include <type_traits>
#include <unordered_map>

#include "hogd/base/status.h"
#include "hogd/base/types.h"
#include "hogd/loadgen/experiment_state.h"
#include "loadgen.hh"
#include "x23/cleanup.h"
#include "x23/mvar.h"

namespace hogd {

class LoadgenServer {
public:
  using rpc_service = LoadgenInterfaceV1;

  LoadgenResult generate_experiment(ExperimentConfig config);
  LoadgenExtensionResult extend_experiment(LoadgenContextID context_id,
                                           uint64_t num_txs);

  ErrnoStatus<uint16_t> start_server(uint16_t port_number = 0);

  ~LoadgenServer()
  {
    listen_thread_.request_stop();
    shutdown(lfd_.get(), SHUT_RDWR);
    lfd_.reset();
  }

private:
  x23::MVar<
      std::unordered_map<LoadgenContextID, std::shared_ptr<ExperimentState>>>
      experiment_states_;
  std::atomic<uint64_t> context_id_counter_;

  x23::UniqueFd lfd_;
  std::jthread listen_thread_;
};

static_assert(is_LoadgenInterfaceV1<LoadgenServer *, std::type_identity_t>);

} // namespace hogd
