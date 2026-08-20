#pragma once

#include <atomic>
#include <cstddef>

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
    EventCard event_card = co_await MempoolEventRpc::wait_for_registration();
    co_return IntroExecResult{.exec_id = exec_id,
                              .shards = shard_cards,
                              .event = event_card};
  }

  static x23::Async<x23::Vec<exec_sched_client_t>>
  get_clients(size_t expected_executors)
  {
    // Executors register from [schedexec_intro_exec()], which the thread pool
    // has to run, so this wait must not occupy a pool worker -- a blocked
    // worker can starve the very handler we are waiting for. Suspend on the
    // executor count instead. See [Registration::registered_executor_count()].
    Registration &reg = Registration::registration();
    bool logged_wait = false;
    while (true) {
      const size_t registered = reg.registered_executor_count();
      if (registered >= expected_executors) {
        break;
      }
      if (!logged_wait) {
        // Startup blocks here until the executors arrive, and there is no
        // deadline, so say so once: otherwise a wedged startup is just
        // [/health] stuck reporting "starting" with nothing in the log. Each
        // executor that does arrive logs itself from [schedexec_intro_exec()].
        info("waiting for executors to register with the coordinator: {} of {} "
             "registered",
             registered, expected_executors);
        logged_wait = true;
      }
      co_await reg.wait_for_executor_count_change(registered);
    }
    x23::Vec<ExecCard> cards;
    reg.executors_registered(&cards);

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
