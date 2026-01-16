#include <concepts>
#include <gtest/gtest.h>

#include "built_contracts/fibonacci.h"
#include "hogd/base/cpputil.h"
#include "hogd/base/types.h"
#include "hogd/hogexec/runtime/contract_trait.h"
#include "hogd/hogexec/runtime/runtime.h"

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
  Runtime::MethodID method_id = 0x1234567890ABCDEF;
  auto entrypoint = entrypoint_from_method_id(method_id);
  ASSERT_EQ(method_id, sdk_method_id(entrypoint));
}

using fibonacci_client_t = ContractTraitMeta<fibonacci::Fibonacci>;
static_assert(
    std::same_as<fibonacci_client_t::return_t<"fibonacci">, uint32_t>);
// static_assert that "invalid_fibonacci" is not a valid method name
// static_assert(!std::same_as<fibonacci_client_t::return_t<"invalid_fibonacci">,
// uint32_t>);

} // namespace hogd
