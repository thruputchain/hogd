#pragma once

#include <memory>

#include "hogd/hogexec/runtime/native_runtime.h"

namespace hogd {

std::unique_ptr<NativeContract> make_native_token_contract();

} // namespace hogd
