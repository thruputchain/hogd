#pragma once

#include <memory>

#include "hogd/hog_exec/runtime/native_runtime.h"

namespace hogd {

std::unique_ptr<NativeContract> make_native_initial_token_claim_contract();

} // namespace hogd
