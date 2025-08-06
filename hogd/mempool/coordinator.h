#pragma once

#include "coord.hh"
#include "coord_sched.hh"

namespace hogd {

// These are fake coordinator servers used by the tests.

class Coordinator {
public:
  using rpc_service = Coordinator1;

  Coordinator() : pool_(/*nthreads=*/4), cp_(pool_) {}

  uint16_t start_server(uint16_t port_number)
  {
    x23::UniqueFd lfd = x23::tcplisten(port_number);
    if (!lfd) {
      perror("listen");
      exit(EXIT_FAILURE);
    }
    // If the [port_number] parameter is 0, then the kernel chooses the port
    // number for us. Thus, in that case, [port_number] will be assigned a
    // nonzero value below.
    port_number = x23::get_port(x23::local_addr(lfd.get()));

    [](x23::CoroPoll &cp, Coordinator *server,
       x23::UniqueFd lfd) -> x23::Detached {
      co_await cp.pool_.bg();
      while (auto c = co_await cp.accept(lfd)) {
        x23::rpc_stream_logged(cp, *c).server(server);
      }
    }(cp_, this, std::move(lfd));

    return port_number;
  }

  static x23::Async<void> coordinator_null() { co_return; }

  static x23::Async<IntroShardResult>
  coordinator_intro_shard(const ShardCard &card)
  {
    co_return IntroShardResult{};
  }

  static x23::Async<IntroExecResult>
  coordinator_intro_exec(const ExecCard &card)
  {
    co_return IntroExecResult{};
  }

private:
  x23::ThreadPool pool_;
  x23::CoroPoll cp_;
};
static_assert(is_Coordinator1<Coordinator *, x23::Async>);

class CoordinatorSched {
public:
  using rpc_service = CoordScheduler1;

  CoordinatorSched() : pool_(/*nthreads=*/4), cp_(pool_) {}

  uint16_t start_server(uint16_t port_number)
  {
    {
      x23::UniqueFd lfd = x23::tcplisten(port_number);
      if (!lfd) {
        perror("listen");
        exit(EXIT_FAILURE);
      }
      // If the [port_number] parameter is 0, then the kernel chooses the port
      // number for us. Thus, in that case, [port_number] will be assigned a
      // nonzero value below.
      port_number = x23::get_port(x23::local_addr(lfd.get()));

      [](x23::CoroPoll &cp, CoordinatorSched *server,
         x23::UniqueFd lfd) -> x23::Detached {
        co_await cp.pool_.bg();
        while (auto c = co_await cp.accept(lfd)) {
          x23::rpc_stream_logged(cp, *c).server(server);
        }
      }(cp_, this, std::move(lfd));
    }

    return port_number;
  }

  static x23::Async<void> coordscheduler_null() { co_return; }

  static x23::Async<void>
  coordscheduler_notify_vetted(const SignedTransactionBatch &txs)
  {
    co_return;
  }

private:
  x23::ThreadPool pool_;
  x23::CoroPoll cp_;
};
static_assert(is_CoordScheduler1<CoordinatorSched *, x23::Async>);

} // namespace hogd
