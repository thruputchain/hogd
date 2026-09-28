#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <span>
#include <utility>
#include <vector>

#include "hogd/base/types.h"
#include "hogd/hog_exec/common/status.h"
#include "hogd/hog_exec/runtime/gas_cost_table.h"
#include "hogd/hog_exec/runtime/syscall_inner.h"
#include "hogd/hog_exec/runtime/syscall_types.h"
#include "syscall_error.hh"
#include "types.hh"

namespace hogd {
namespace {

// VerifySignature must reserve the message-length-dependent Ed25519 cost before
// running the verifier. A frame that cannot afford it faults with OutOfGas
// without performing any curve work, so a low-gas child frame can no longer run
// full verification for the price of the syscall crossing (issue #4172).
TEST(VerifySignatureGasTest, ReservesVerificationCostBeforeRunning)
{
  const std::array<std::byte, sizeof(Ed25519PublicKey)> public_key{};
  const std::array<std::byte, sizeof(Ed25519Signature)> signature{};
  const std::vector<std::byte> message(1024, std::byte{0});
  const auto scheme = std::to_underlying(CryptographicScheme::Ed25519);
  const GasAmount required = GasCost::verify_signature(message.size());

  // Budgets at or below the required cost fault before verifying. Exact
  // exhaustion faults too, matching the outer wrapper and the SHA256 path.
  for (const GasAmount budget : {GasAmount{0}, required - 1, required}) {
    SCOPED_TRACE(::testing::Message() << "budget=" << budget);
    const auto [result, charge] = syscall_VerifySignature_inner(
        WrappedGasAmount{budget}, scheme, signature, public_key, message);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code(), HostFnErrorCode::OutOfGas);
    EXPECT_EQ(charge.get(), budget);
  }

  // With enough gas the verifier runs and is charged the full length-based
  // cost, whatever the verification outcome.
  const auto [result, charge] = syscall_VerifySignature_inner(
      WrappedGasAmount{required + 1}, scheme, signature, public_key, message);
  EXPECT_TRUE(result.has_value());
  EXPECT_EQ(charge.get(), required);
}

} // namespace
} // namespace hogd
