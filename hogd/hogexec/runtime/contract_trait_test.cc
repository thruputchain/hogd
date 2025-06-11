#include <gtest/gtest.h>

#include "hogd/hogexec/runtime/contract_trait.h"
#include "types.hh"

namespace hogd {

TEST(ContractTraitTest, MethodIDDerivation)
{
  auto id = sdk_method_id("hogd", "0.1.0", 0x1234);
  auto id2 = sdk_method_id("hogd", "0.1.0", 0xABCD);
  ASSERT_EQ(id & 0xFFFF'FFFF'FFFF'0000, id2 & 0xFFFF'FFFF'FFFF'0000);
  ASSERT_NE(id & 0x0000'0000'0000'FFFF, id2 & 0x0000'0000'0000'FFFF);
}

TEST(ContractTraitTest, EntrypointFromMethodID)
{
  MethodID method_id = 0x1234567890ABCDEF;
  auto entrypoint = entrypoint_from_method_id(method_id);
  ASSERT_EQ(method_id, sdk_method_id(entrypoint));
}

} // namespace hogd
