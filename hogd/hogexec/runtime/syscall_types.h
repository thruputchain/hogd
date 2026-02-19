#pragma once

#include "types.hh"

namespace hogd {

class WrappedGasAmount {
public:
  constexpr explicit WrappedGasAmount(GasAmount gas_amount)
    : gas_amount_(gas_amount)
  {}
  constexpr GasAmount get() const { return gas_amount_; }

private:
  GasAmount gas_amount_;
};

} // namespace hogd
