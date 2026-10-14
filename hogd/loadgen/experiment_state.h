#pragma once

#include "hogd/base/types.h"
#include "hogd/loadgen/status.h"
#include "loadgen.hh"
#include "types.hh"
#include "x23/async.h"

namespace hogd {

// Experiment state that can be extended for more transactions

class ExperimentState {
public:
  // May be called concurrently after generate_experiment publishes the state.
  virtual x23::Async<LoadgenStatus<LoadgenExtension>>
  extend_experiment(uint64_t num_txs, BlockHeight expiry_height) const = 0;

  virtual ~ExperimentState() = default;

private:
};

} // namespace hogd
