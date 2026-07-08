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
// must hash the charset-independent u8"" preimage twins, not the ordinary
// literals whose bytes depend on -fexec-charset.
//
// This toolchain's clang only accepts -fexec-charset=UTF-8, so we can't produce
// a genuine non-UTF-8 ordinary literal. Instead we model the exact divergence a
// non-UTF-8 build creates: the ordinary kTraitName/kVersion are deliberately
// corrupted so their rehash differs from the UTF-8-derived kEntrypoints, while
// the u8"" twins hold the correct UTF-8 preimage. kLookupIndex still matches
// "transfer" within this TU (kExportNames is unchanged), exactly as it would in
// a real -fexec-charset build. Reproduces on any execution charset.
namespace {
struct Utf8CrossCheckTrait {
  // Corrupted ordinary literals: stand in for non-UTF-8 execution-charset bytes.
  constexpr static std::string_view kTraitName = "XRC20";
  constexpr static std::string_view kVersion = "9.9.9";
  constexpr static auto kExportNames =
      std::array<std::string_view, 1>{"transfer"};

  // UTF-8-derived source of truth: sha256("ERC20::0.1::transfer")[:8].
  constexpr static auto kEntrypoints =
      std::array<uint64_t, 1>{0x63285b3523f9dc3fULL};

  // Correct UTF-8 twins (what generate_header.py now emits).
  constexpr static std::u8string_view kTraitNameUtf8 = u8"ERC20";
  constexpr static std::u8string_view kVersionUtf8 = u8"0.1.4";
  constexpr static auto kExportNamesUtf8 =
      std::array<std::u8string_view, 1>{u8"transfer"};

  using export_types = std::tuple<void (*)()>;
};
} // namespace

TEST(ContractTraitTest, EntrypointDebugCrossCheckUsesUtf8Twins)
{
  using meta = ContractTraitMeta<Utf8CrossCheckTrait>;
  auto canonical =
      entrypoint_from_method_id(Utf8CrossCheckTrait::kEntrypoints[0]);

  // Precondition: the ordinary-literal rehash (what the pre-#2898 cross-check
  // hashed) diverges from the UTF-8-derived kEntrypoints. This is the exact
  // condition a non-UTF-8 -fexec-charset build produces.
  ASSERT_NE(canonical,
            sdk_entrypoint_derivation(Utf8CrossCheckTrait::kTraitName,
                                      Utf8CrossCheckTrait::kVersion,
                                      "transfer"));

  // The u8"" twins reproduce the canonical id independent of execution charset.
  ASSERT_EQ(canonical,
            sdk_entrypoint_derivation(Utf8CrossCheckTrait::kTraitNameUtf8,
                                      Utf8CrossCheckTrait::kVersionUtf8,
                                      Utf8CrossCheckTrait::kExportNamesUtf8[0]));

  // Therefore entrypoint() no longer aborts on the debug cross-check and
  // returns the canonical id. Against the pre-#2898 code this call SIGABRTs.
  ASSERT_EQ(meta::entrypoint<"transfer">(), canonical);
}

} // namespace hogd
