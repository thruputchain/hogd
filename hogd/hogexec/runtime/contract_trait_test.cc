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

  // major > 0: fold keeps only the major component (ignores minor/patch), so
  // preimage is "hogd::2::abc".
  auto maj = sdk_method_id("hogd", "2.5.7", "abc");
  ASSERT_EQ(maj, sdk_method_id("hogd", "2.9.9", "abc"));
  ASSERT_NE(maj, sdk_method_id("hogd", "3.0.0", "abc"));
  ASSERT_EQ(maj, Runtime::MethodID{0x69e35d2fdb6e7e55ULL});

  // major == minor == 0, patch > 0: fold keeps "0.0.PATCH" and drops any
  // -prerelease/+build, so preimage is "hogd::0.0.7::abc".
  auto pat = sdk_method_id("hogd", "0.0.7", "abc");
  ASSERT_EQ(pat, sdk_method_id("hogd", "0.0.7-rc.1", "abc"));
  ASSERT_NE(pat, sdk_method_id("hogd", "0.0.8", "abc"));
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
    std::same_as<fibonacci_client_t::return_t<"fibonacci">, uint32_t>);
// static_assert that "invalid_fibonacci" is not a valid method name
// static_assert(!std::same_as<fibonacci_client_t::return_t<"invalid_fibonacci">,
// uint32_t>);

// Regression for #2898: the assertion-enabled entrypoint() debug cross-check
// must hash the charset-independent, already-folded u8"" preimage the generator
// emits, not re-fold ordinary literals through the semver parser (whose regex
// and "0" literals depend on -fexec-charset).
//
// This toolchain's clang only accepts -fexec-charset=UTF-8, so we can't produce
// a genuine non-UTF-8 ordinary literal. Instead we model the exact divergence a
// non-UTF-8 build creates: the ordinary kTraitName/kVersion are deliberately
// corrupted so re-folding them differs from the UTF-8-derived kEntrypoints,
// while kEntrypointPreimagesUtf8 holds the correct folded UTF-8 preimage.
// kLookupIndex still matches "transfer" within this TU (kExportNames is
// unchanged), exactly as it would in a real -fexec-charset build. Reproduces on
// any execution charset.
namespace {
struct Utf8CrossCheckTrait {
  // Corrupted ordinary literals: stand in for non-UTF-8 execution-charset
  // bytes.
  constexpr static std::string_view kTraitName = "XRC20";
  constexpr static std::string_view kVersion = "9.9.9";
  constexpr static auto kExportNames =
      std::array<std::string_view, 1>{"transfer"};

  // UTF-8-derived source of truth: sha256("ERC20::0.1::transfer")[:8].
  constexpr static auto kEntrypoints =
      std::array<uint64_t, 1>{0x63285b3523f9dc3fULL};

  // Correct folded UTF-8 preimage (what generate_header.py now emits).
  constexpr static auto kEntrypointPreimagesUtf8 =
      std::array<std::u8string_view, 1>{u8"ERC20::0.1::transfer"};

  using export_types = std::tuple<void (*)()>;
};
} // namespace

TEST(ContractTraitTest, EntrypointDebugCrossCheckUsesUtf8Preimage)
{
  using meta = ContractTraitMeta<Utf8CrossCheckTrait>;
  auto canonical =
      entrypoint_from_method_id(Utf8CrossCheckTrait::kEntrypoints[0]);

  // Precondition: re-folding the ordinary literals (what the pre-#2898
  // cross-check did) diverges from the UTF-8-derived kEntrypoints. This is the
  // exact condition a non-UTF-8 -fexec-charset build produces.
  ASSERT_NE(canonical,
            sdk_entrypoint_derivation(Utf8CrossCheckTrait::kTraitName,
                                      Utf8CrossCheckTrait::kVersion,
                                      "transfer"));

  // Hashing the folded u8"" preimage reproduces the canonical id independent of
  // execution charset and of the semver parser.
  ASSERT_EQ(canonical, entrypoint_id_from_utf8_preimage(
                           Utf8CrossCheckTrait::kEntrypointPreimagesUtf8[0]));

  // Therefore entrypoint() no longer aborts on the debug cross-check and
  // returns the canonical id. Against the pre-#2898 code this call SIGABRTs.
  ASSERT_EQ(meta::entrypoint<"transfer">(), canonical);
}

} // namespace hogd
