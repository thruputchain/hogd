#pragma once

#include <memory>

#include "hogd/hog_exec/runtime/native_runtime.h"

namespace hogd {

// Native mirror of the default_wallet reference contract
// (built_contracts/integration_test_contracts/default_wallet). Byte-for-byte
// observable parity with the wasm build is the contract; see
// default_wallet_parity_test.cc. Not registered in any production context.
std::unique_ptr<NativeContract> make_native_default_wallet_contract();

} // namespace hogd
