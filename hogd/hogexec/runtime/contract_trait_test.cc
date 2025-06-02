#include <gtest/gtest.h>

#include "hogd/hogexec/runtime/contract_trait.h"

namespace hogd {

TEST(ContractTraitTest, MethodIDDerivation)
{
  auto id = sdk_method_id("hogd", "0.1.0", 0x1234);
  auto id2 = sdk_method_id("hogd", "0.1.0", 0xABCD);
  ASSERT_EQ(id & 0x0000'FFFF'FFFF'FFFF, id2 & 0x0000'FFFF'FFFF'FFFF);
  ASSERT_NE(id & 0xFFFF'0000'0000'0000, id2 & 0xFFFF'0000'0000'0000);
}

} // namespace hogd
