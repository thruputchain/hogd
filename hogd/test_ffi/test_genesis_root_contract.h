#pragma once

// The one shared genesis-root contract used by the test_ffi test suites.

#include <cstdint>

#include "contract.hh"
#include "hogd/test_ffi/test_bindings.h"

namespace hogd {

// Mock contract installed as the genesis root: the tests drive contract
// bodies through the mock trampoline, so the genesis root's code never runs
// and any deployable contract satisfies environment creation. Uses the
// reserved mock id so it can never collide with a registered test mock.
inline Contract
make_test_genesis_root_contract()
{
  Contract contract(ContractType::kMockTest);
  contract.mock_contract_id() =
      static_cast<int32_t>(TEST_FFI_GENESIS_ROOT_MOCK_CONTRACT_ID);
  return contract;
}

} // namespace hogd
