#pragma once

#include <atomic>

#include "exec_sched.hh"
#include "hogd/base/log.h"
#include "hogd/base/rpc_server.h"
#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h" // IWYU pragma: keep
#include "hogd/mempool/registration.h"
#include "sched_exec.hh"
#include "types.h"
#include "x23/async.h"
#include "x23/cleanup.h"
#include "x23/rpc.h"
#include "x23/sockio.h"
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
    info("executor with card {} connected. {} total executors connected.",
         x23::show(card),
         // We add 1 because we print this before we actually add the executor
         // below.
         Registration::registration().executors_registered(/*cards=*/nullptr) +
             1);

    size_t exec_id = Registration::registration().register_executor(card);
    x23::Vec<ShardCard> shard_cards;
    CHECK_GT(Registration::registration().shards_registered(&shard_cards), 0UL);
    EventCard event_card;
    CHECK(Registration::registration().event_registered(&event_card));
    co_return IntroExecResult{.exec_id = exec_id,
                              .shards = shard_cards,
                              .event = event_card};
  }

  static x23::Vec<exec_sched_client_t> get_clients(x23::CoroPoll &cp)
  {
    x23::Vec<ExecCard> cards;
    Registration::registration().executors_registered(&cards);

    x23::Vec<exec_sched_client_t> ret;
    ret.reserve(cards.size());

    for (const ExecCard &card : cards) {
      x23::UniqueFd fd =
          x23::tcpconnect(server_address_to_sockaddr(card.address));
      ret.push_back(
          x23::rpc_stream_logged(cp, fd.release())->client(ExecutorSched1{}));
    }
    return ret;
  }

  void shut_down()
  {
    // TODO(jhumphri): Implement.
    shutdown_flag_ = true;
  }

  void await_shutdown_request() { shutdown_flag_.wait(false); }

private:
  std::atomic<bool> shutdown_flag_ = false;
};
static_assert(is_SchedExec1<MempoolExecutorRpc *, x23::Async>);

} // namespace hogd
