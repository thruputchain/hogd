#pragma once

#include "hogd/base/types.h"
#include "hogd/loadgen/status.h"
#include "loadgen.hh"

namespace hogd {

// Experiment state that can be extended for more transactions

class ExperimentState {
public:
  virtual LoadgenStatus<LoadgenExtension>
  extend_experiment(uint64_t num_txs) = 0;

  virtual ~ExperimentState() = default;

private:
};

} // namespace hogd
