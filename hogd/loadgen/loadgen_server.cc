#include "loadgen_server.h"

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <stop_token>
#include <sys/socket.h>
#include <thread>
#include <utility>

#include "hogd/base/log.h"
#include "hogd/base/status.h"
#include "hogd/base/types.h"
#include "hogd/loadgen/erc20_experiment.h"
#include "loadgen.hh"
#include "x23/cleanup.h"
#include "x23/sockio.h"
#include "x23/srpc.h"

namespace hogd {

ErrnoStatus<uint16_t>
LoadgenServer::start_server(uint16_t port_number)
{
  lfd_ = x23::stcplisten(port_number);
  if (!lfd_) {
    perror("listen");
    exit(EXIT_FAILURE);
  }
  uint16_t port = x23::get_port(x23::local_addr(lfd_.get()));

  listen_thread_ = std::jthread([this](std::stop_token &&stop) {
    while (!stop.stop_requested()) {

      sockaddr_storage ss;
      socklen_t sslen = sizeof(ss);
      x23::UniqueFd fd{
          accept(lfd_.get(), reinterpret_cast<sockaddr *>(&ss), &sslen)};
      if (!fd) {
        info("accept error: {}", Errno{});
      }
      else {
        x23::srpc_stream_logged(fd.release())->server(this);
      }
    }
  });

  return port;
}

LoadgenResult
LoadgenServer::generate_experiment(ExperimentConfig config)
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
LoadgenServer::extend_experiment(LoadgenContextID context_id, uint64_t num_txs)
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
