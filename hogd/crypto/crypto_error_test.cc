#include <gtest/gtest.h>

#include "hogd/crypto/crypto_error.h"
#include "hogd/crypto/initialize.h"

#include <format>

namespace hogd {

TEST(CryptoError, OutOfRange)
{
  EXPECT_EQ(std::format("{}", CryptoError::category().message(500)),
            "unknown out of range");
}

TEST(CryptoError, Initialization)
{
  EXPECT_TRUE(initialize_crypto());
}

} // namespace hogd
