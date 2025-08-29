#pragma once

#include "hogd/loadgen/status.h"
#include "loadgen.hh"

namespace hogd {

LoadgenStatus<LoadgenSuccessResult>
generate_erc20_payments_experiment(const ERC20PaymentsConfig &config);

} // namespace hogd
