#pragma once

#include <atomic>
#include <memory>
#include <type_traits>
#include <unordered_map>

#include "hogd/base/rpc_server.h"
#include "hogd/base/types.h"
#include "hogd/loadgen/experiment_state.h"
#include "loadgen.hh"
#include "types.hh"
#include "x23/async.h"
#include "x23/mvar.h"

namespace hogd {

class LoadgenRpc {
public:
  using rpc_service = LoadgenInterfaceV1;

  // We set the RPC max message size to the largest value x23 supports because
  // the loadgen transfers a lot of data via individual RPC calls.  This is the
  // ONC RPC single-fragment record limit; x23 cannot frame a larger message.
  static constexpr size_t kRpcMaxMessageSize = 0x7fffffff;

  // Size of the synthetic recipient address space handed to every experiment
  // this server generates; zero keeps transfers inside the wallet set. Set
  // once at startup, before the server accepts calls.
  uint64_t num_virtual_addresses_ = 0;

  // State-filler knobs, server-side for the same reason as
  // num_virtual_addresses_: changing them needs no change to the loadgen RPC.
  // Zero instances disables the filler. See ERC20ExperimentState.
  uint32_t filler_instances_ = 0;
  uint32_t filler_items_ = 0;
  uint32_t filler_item_bytes_ = 0;
  uint32_t filler_period_ = 1;
  uint64_t filler_keyspace_ = 0;

  x23::Async<LoadgenResult> generate_experiment(ExperimentConfig config);

  x23::Async<LoadgenExtensionResult>
  extend_experiment(LoadgenContextID context_id, uint64_t num_txs,
                    BlockHeight expiry_height);

  void shut_down()
  {
    // TODO(jhumphri): Implement.
  }

private:
  x23::MVar<
      std::unordered_map<LoadgenContextID, std::shared_ptr<ExperimentState>>>
      experiment_states_;
  std::atomic<uint64_t> context_id_counter_;
};

static_assert(is_LoadgenInterfaceV1<LoadgenRpc *, x23::Async>);

} // namespace hogd
