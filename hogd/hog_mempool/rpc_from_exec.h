#pragma once

#include <atomic>
#include <chrono>
#include <thread>

#include "exec_sched.hh"
#include "hogd/base/log.h"
#include "hogd/base/rpc_server.h"
#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h" // IWYU pragma: keep
#include "hogd/hog_exec/interface/executor.h"
#include "hogd/hog_mempool/registration.h"
#include "hogd/hog_mempool/shared.h"
#include "sched_db.hh"
#include "sched_event.hh"
#include "sched_exec.hh"
#include "x23/async.h"
#include "x23/format.h"
#include "x23/vec.h"

namespace hogd {

class MempoolExecutorRpc {
public:
  using rpc_service = SchedExec1;

  MempoolExecutorRpc() = default;

  // Disable copy and move.
  MempoolExecutorRpc(const MempoolExecutorRpc &) = delete;
  MempoolExecutorRpc &operator=(const MempoolExecutorRpc &) = delete;
  MempoolExecutorRpc(MempoolExecutorRpc &&) = delete;
  MempoolExecutorRpc &operator=(MempoolExecutorRpc &&) = delete;

  static x23::Async<IntroExecResult> schedexec_intro_exec(ExecCard card)
  {
    size_t exec_id = Registration::registration().register_executor(card);
    info("executor with card {} connected as {}. {} total executors connected.",
         x23::show(card), exec_id,
         Registration::registration().executors_registered(/*cards=*/nullptr));
    x23::XdrVec<ShardCard> shard_cards;
    CHECK_GT(Registration::registration().shards_registered(&shard_cards), 0UL);
    co_await default_threadpool().bg();
    EventCard event_card = MempoolEventRpc::wait_for_registration();
    co_return IntroExecResult{.exec_id = exec_id,
                              .shards = shard_cards,
                              .event = event_card};
  }

  // Waits for [expected_executors] executors to register, then connects to
  // each one.
  //
  // Executors register asynchronously once the coordinator is listening, so
  // there is no upper bound on how long this legitimately takes: the mempool
  // has nothing to serve until they arrive, which is precisely the state
  // [Health::Status::kStarting] reports to whoever is supervising startup (see
  // [MempoolCoordinatorRpc::finish_runtime_init()]). Wait for them rather than
  // aborting the process on a wall-clock deadline, matching how the same
  // startup path waits for the event server in
  // [MempoolEventRpc::wait_for_registration()]. Log while waiting so a
  // registration that is genuinely stuck is visible in the logs.
  static x23::Async<x23::Vec<exec_sched_client_t>>
  get_clients(size_t expected_executors)
  {
    co_await default_threadpool().bg();
    while (true) {
      const size_t registered =
          Registration::registration().executors_registered(nullptr);
      if (registered >= expected_executors) {
        break;
      }
      warn_every_n_sec(5.0F,
                       "waiting for executors to register with the "
                       "coordinator: {} of {} registered",
                       registered, expected_executors);
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    x23::Vec<ExecCard> cards;
    Registration::registration().executors_registered(&cards);

    x23::Vec<exec_sched_client_t> ret;
    ret.reserve(cards.size());

    for (const ExecCard &card : cards) {
      auto client = co_await create_client<ExecutorSched1>(
          server_address_to_sockaddr(card.address), /*retry=*/false,
          /*max_message_size=*/Executor::kMessageSize);
      if (!client)
        panic("rpc_from_exec get_clients connect failed: {}",
              client.error().error_code().message());
      ret.push_back(std::move(*client));
    }
    co_return ret;
  }

  void shut_down()
  {
    // TODO(jhumphri): Implement.
    shutdown_flag_ = true;
  }

private:
  std::atomic<bool> shutdown_flag_ = false;
};
static_assert(is_SchedExec1<MempoolExecutorRpc *, x23::Async>);

} // namespace hogd
