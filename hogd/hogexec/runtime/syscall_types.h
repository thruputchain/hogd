#pragma once

#include "types.hh"

namespace hogd {

struct WrappedGasAmount {
  [[no_unique_address]] GasAmount gas_amount_;
};

} // namespace hogd
