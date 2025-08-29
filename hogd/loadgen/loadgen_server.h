#pragma once

#include <sys/socket.h>
#include <thread>
#include <type_traits>

#include "hogd/base/status.h"
#include "hogd/base/types.h"
#include "loadgen.hh"
#include "x23/cleanup.h"

namespace hogd {

class LoadgenServer {
public:
  using rpc_service = LoadgenInterfaceV1;

  LoadgenResult generate_experiment(ExperimentConfig config);

  ErrnoStatus<uint16_t> start_server(uint16_t port_number = 0);

  ~LoadgenServer()
  {
    listen_thread_.request_stop();
    shutdown(lfd_.get(), SHUT_RDWR);
    lfd_.reset();
  }

private:
  x23::UniqueFd lfd_;
  std::jthread listen_thread_;
};

static_assert(is_LoadgenInterfaceV1<LoadgenServer *, std::type_identity_t>);

} // namespace hogd
