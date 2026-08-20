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
    // [wait_for_registration()] polls with a blocking sleep, so it has to run
    // off the pool. See the comment in [get_clients()].
    EventCard event_card = co_await default_threadpool().async(
        [] { return MempoolEventRpc::wait_for_registration(); });
    co_return IntroExecResult{.exec_id = exec_id,
                              .shards = shard_cards,
                              .event = event_card};
  }

  static x23::Async<x23::Vec<exec_sched_client_t>>
  get_clients(size_t expected_executors)
  {
    // This wait must not run on a thread-pool worker. Executors register from
    // [schedexec_intro_exec()], which the pool has to run, so blocking a worker
    // here can starve the very registration we are waiting for: a worker that
    // is the last one servicing its runqueue cannot migrate away, and the other
    // workers park permanently on their own empty queues rather than stealing.
    // The intro handler then sits at the head of that runqueue until the
    // deadline expires and the CHECK below aborts the process. Run the blocking
    // poll on a dedicated off-pool thread instead.
    co_await default_threadpool().async([expected_executors] {
      const auto deadline =
          std::chrono::steady_clock::now() + std::chrono::seconds(30);
      while (Registration::registration().executors_registered(nullptr) <
                 expected_executors &&
             std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
      }
    });
    CHECK_GE(Registration::registration().executors_registered(nullptr),
             expected_executors);
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
