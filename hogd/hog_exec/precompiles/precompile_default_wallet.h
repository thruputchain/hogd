#pragma once

#include <memory>

#include "hogd/hog_exec/runtime/native_runtime.h"

namespace hogd {

// Native mirror of the default_wallet reference contract
// (built_contracts/integration_test_contracts/default_wallet). Byte-for-byte
// observable parity with the wasm build is the contract; see
// default_wallet_parity_test.cc. Registered in make_hog_exec_precompiles_context()
// as kNativeContractDefaultWallet; reachable only via root-resolution fallback.
std::unique_ptr<NativeContract> make_native_default_wallet_contract();

} // namespace hogd
