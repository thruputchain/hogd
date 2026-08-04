#include <array>
#include <concepts>
#include <string_view>
#include <tuple>

#include <gtest/gtest.h>

#include "built_contracts/fibonacci.h"
#include "hogd/base/cpputil.h"
#include "hogd/base/types.h"
#include "hogd/hogexec/runtime/contract_trait.h"
#include "hogd/hogexec/runtime/runtime.h"

namespace hogd {

TEST(ContractTraitTest, MethodIDDerivation)
{
  auto id = sdk_method_id(u8"hogd", u8"0.1.0", u8"abc");
  auto id2 = sdk_method_id(u8"hogd", u8"0.1.12", u8"abc");
  ASSERT_EQ(id, id2);

  auto id3 = sdk_method_id(u8"hogd", u8"0.1.0", u8"def");
  ASSERT_NE(id, id3);

  auto id4 = sdk_method_id(u8"hogd", u8"0.1.0-alpha", u8"abc");
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
  auto z_alpha = sdk_method_id(u8"hogd", u8"0.0.0-alpha", u8"abc");
  auto z_beta = sdk_method_id(u8"hogd", u8"0.0.0-beta", u8"abc");
  auto z_plain = sdk_method_id(u8"hogd", u8"0.0.0", u8"abc");
  ASSERT_NE(z_alpha, z_beta);
  ASSERT_NE(z_alpha, z_plain);
  // Golden vector: preimage "hogd::0.0.0-alpha::abc".
  ASSERT_EQ(z_alpha, Runtime::MethodID{0x99b536b207ea02cfULL});

  // major > 0: fold keeps only the major component (ignores minor/patch), so
  // preimage is "hogd::2::abc".
  auto maj = sdk_method_id(u8"hogd", u8"2.5.7", u8"abc");
  ASSERT_EQ(maj, sdk_method_id(u8"hogd", u8"2.9.9", u8"abc"));
  ASSERT_NE(maj, sdk_method_id(u8"hogd", u8"3.0.0", u8"abc"));
  ASSERT_EQ(maj, Runtime::MethodID{0x69e35d2fdb6e7e55ULL});

  // major == minor == 0, patch > 0: fold keeps "0.0.PATCH" and drops any
  // -prerelease/+build, so preimage is "hogd::0.0.7::abc".
  auto pat = sdk_method_id(u8"hogd", u8"0.0.7", u8"abc");
  ASSERT_EQ(pat, sdk_method_id(u8"hogd", u8"0.0.7-rc.1", u8"abc"));
  ASSERT_NE(pat, sdk_method_id(u8"hogd", u8"0.0.8", u8"abc"));
  ASSERT_EQ(pat, Runtime::MethodID{0xa6e0c6d93a7f2c46ULL});
}

TEST(ContractTraitTest, EntrypointFromMethodID)
{
  Runtime::MethodID method_id = 0x1234567890ABCDEF;
  auto entrypoint = entrypoint_from_method_id(method_id);
  ASSERT_EQ(method_id, sdk_method_id(entrypoint));
}

using fibonacci_client_t = ContractTraitMeta<fibonacci::Fibonacci>;
static_assert(
    std::same_as<fibonacci_client_t::return_t<u8"fibonacci">, uint32_t>);
// static_assert that "invalid_fibonacci" is not a valid method name
// static_assert(!std::same_as<fibonacci_client_t::return_t<u8"invalid_fibonacci">,
// uint32_t>);

// #2898: generated trait metadata must be u8"" so entrypoint-id derivation
// hashes UTF-8 bytes independent of -fexec-charset. Pin the type here; a
// regression to ordinary string_view would fail to compile.
static_assert(std::same_as<decltype(fibonacci::Fibonacci::kTraitName),
                           const std::u8string_view>);
static_assert(
    std::same_as<decltype(fibonacci::Fibonacci::kExportNames),
                 const std::array<std::u8string_view,
                                  fibonacci::Fibonacci::kExportNames.size()>>);

// Regression for #2898: the entrypoint() debug cross-check has the host
// independently re-derive from the trait's u8"" metadata and match the
// generated kEntrypoints. Because the metadata is u8"" (not ordinary literals)
// the hashed bytes are UTF-8 regardless of -fexec-charset, so the two agree on
// any execution charset instead of aborting on a non-UTF-8 build.
namespace {
struct Utf8CrossCheckTrait {
  constexpr static std::u8string_view kTraitName = u8"ERC20";
  constexpr static std::u8string_view kVersion = u8"0.1.4";
  constexpr static auto kExportNames =
      std::array<std::u8string_view, 1>{u8"transfer"};

  // UTF-8-derived source of truth: sha256("ERC20::0.1::transfer")[:8].
  constexpr static auto kEntrypoints =
      std::array<uint64_t, 1>{0x63285b3523f9dc3fULL};

  using export_types = std::tuple<void (*)()>;
};
} // namespace

TEST(ContractTraitTest, EntrypointDebugCrossCheckDerivesFromUtf8Metadata)
{
  using meta = ContractTraitMeta<Utf8CrossCheckTrait>;
  auto canonical =
      entrypoint_from_method_id(Utf8CrossCheckTrait::kEntrypoints[0]);

  // The host independently re-derives from the u8"" metadata and reproduces the
  // generated kEntrypoints -- charset-independent because the inputs are u8"".
  ASSERT_EQ(canonical,
            sdk_entrypoint_derivation(Utf8CrossCheckTrait::kTraitName,
                                      Utf8CrossCheckTrait::kVersion,
                                      Utf8CrossCheckTrait::kExportNames[0]));

  // So the entrypoint() cross-check holds and returns the canonical id. Against
  // the pre-#2898 code (ordinary literals) this diverged and aborted under a
  // non-UTF-8 -fexec-charset.
  ASSERT_EQ(meta::entrypoint<u8"transfer">(), canonical);
}

} // namespace hogd
