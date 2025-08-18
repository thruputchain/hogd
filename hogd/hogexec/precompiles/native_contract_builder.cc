#include "hogd/hogexec/precompiles/native_contract_builder.h"

#include <cassert>
#include <memory>
#include <utility>

#include "hogd/hogexec/runtime/native_runtime.h"

namespace hogd {

std::unique_ptr<NativeContract>
NativeContractBuilder::build()
{
  assert(!built_);
  built_ = true;
  return std::make_unique<NativeContract>(std::move(functions_));
}

} // namespace hogd
