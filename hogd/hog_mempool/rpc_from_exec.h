#pragma once

#include <atomic>
#include <cstddef>
#include <string_view>

#include "exec_sched.hh"
#include "hogd/base/log.h"
#include "hogd/base/rpc_server.h"
#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h" // IWYU pragma: keep
#include "hogd/hog_exec/interface/executor.h"
#include "hogd/hog_mempool/registration.h"
#include "hogd/hog_mempool/shared.h"
#include "hogd/version.h"
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
    // The reply hands back every ShardCard, so a skewed executor would decode
    // them to a different layout. IntroExecResult has no outcome to reject
    // with, so this is fatal.
    if (std::string_view(card.protocol) !=
        std::string_view(version::kHogdProtocol())) {
      panic("executor protocol {} does not match {}; refusing to register",
            card.protocol.c_str(), version::kHogdProtocol());
    }
    info("executor with card {} connected. {} total executors connected.",
         x23::show(card),
         // We add 1 because we print this before we actually add the executor
         // below.
         Registration::registration().executors_registered(/*cards=*/nullptr) +
             1);

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
    // Suspending, not blocking, matters here: executors register from
    // [schedexec_intro_exec()], which the pool has to run. See [Registration].
    Registration &reg = Registration::registration();
    const size_t registered = reg.executors_registered(/*cards=*/nullptr);
    if (registered < expected_executors) {
      // There is no deadline, so leave a breadcrumb for a wedged startup.
      info("waiting for executors to register with the coordinator: {} of {} "
           "registered",
           registered, expected_executors);
      co_await reg.wait_for_executors(expected_executors);
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
