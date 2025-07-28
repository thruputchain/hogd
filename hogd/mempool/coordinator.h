#pragma once

#include "coord.hh"

namespace hogd {

class CoordinatorServer {
public:
  using rpc_service = Coordinator1;

  CoordinatorServer(uint16_t port_number) : pool_(/*nthreads=*/4), cp_(pool_)
  {
    start_server(port_number);
  }

  void start_server(uint32_t port_number)
  {
    {
      auto lfd = x23::tcplisten(port_number);
      if (!lfd) {
        perror("listen");
        exit(EXIT_FAILURE);
      }

      [](x23::CoroPoll &cp, CoordinatorServer *server,
         x23::UniqueFd lfd) -> x23::Detached {
        co_await cp.pool_.bg();
        while (auto c = co_await cp.accept(lfd)) {
          x23::rpc_stream_logged(cp, *c).server(server);
        }
      }(cp_, this, std::move(lfd));
    }
  }

  x23::Async<void> coordinator_null() { co_return; }

  x23::Async<IntroShardResult> coordinator_intro_shard(ShardCard card)
  {
    co_return IntroShardResult{};
  }

  x23::Async<IntroExecResult> coordinator_intro_exec(ExecCard card)
  {
    co_return IntroExecResult{};
  }

private:
  x23::ThreadPool pool_;
  x23::CoroPoll cp_;
};

} // namespace hogd
