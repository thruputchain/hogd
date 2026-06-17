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
  auto id = sdk_method_id("hogd", "0.1.0", "abc");
  auto id2 = sdk_method_id("hogd", "0.1.12", "abc");
  ASSERT_EQ(id, id2);

  auto id3 = sdk_method_id("hogd", "0.1.0", "def");
  ASSERT_NE(id, id3);

  auto id4 = sdk_method_id("hogd", "0.1.0-alpha", "abc");
  ASSERT_EQ(id, id4);

  // Golden vector pinning the exact preimage format:
  //   sha256("<name>::<major_version>::<method>"), first 8 bytes -> method id.
  // For ("hogd", "0.1.0", "abc") the preimage is "hogd::0.1::abc". This must
  // byte-match the SDK's derivation; do not update it to match a code change
  // without confirming the SDK derives the same value.
  ASSERT_EQ(id, Runtime::MethodID{0x9432b95f6309d52bULL});

  // 0.0.0 versions use the full original version string (prerelease and build
  // included) as the major-version component, matching the SDK and
  // generate_header.py. Distinct prereleases therefore derive distinct ids, and
  // the leading '-' is part of the preimage (not stripped).
  auto z_alpha = sdk_method_id("hogd", "0.0.0-alpha", "abc");
  auto z_beta = sdk_method_id("hogd", "0.0.0-beta", "abc");
  auto z_plain = sdk_method_id("hogd", "0.0.0", "abc");
  ASSERT_NE(z_alpha, z_beta);
  ASSERT_NE(z_alpha, z_plain);
  // Golden vector: preimage "hogd::0.0.0-alpha::abc".
  ASSERT_EQ(z_alpha, Runtime::MethodID{0x99b536b207ea02cfULL});
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
