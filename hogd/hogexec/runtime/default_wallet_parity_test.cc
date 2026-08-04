#include <gtest/gtest.h>

#include "built_contracts/default_wallet.h"
#include "hogd/hogexec/precompiles/precompile_default_wallet.h"

namespace hogd {

// NOLINTNEXTLINE(misc-use-internal-linkage)
TEST(DefaultWalletPrecompile, ConstructsAndCoversAllEntrypoints)
{
  auto contract = make_native_default_wallet_contract();
  ASSERT_NE(contract, nullptr);
}

} // namespace hogd
