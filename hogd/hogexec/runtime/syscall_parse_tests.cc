#include <gtest/gtest.h>

#include <array>
#include <cstring>
#include <limits>
#include <span>
#include <vector>

#include "auth_claim.hh"
#include "hogd/base/types.h"
#include "hogd/hogexec/components/alias_guard.h"
#include "hogd/hogexec/components/ffi_helpers.h"
#include "hogd/hogexec/components/ffi_types.h"
#include "hogd/hogexec/runtime/syscall_arg_params.h"
#include "hogd/hogexec/runtime/syscall_argument_parsers.h"
#include "types.hh"

namespace hogd {

namespace {

enum class TestUnsignedEnum : uint8_t {
  kZero = 0,
  kMax = 0xFF,
};

enum class TestSignedEnum : int32_t {
  kZero = 0,
};

} // namespace

// These tests cover the wasm32 `parse()` path, where every value slot is a bare
// 32-bit word (uint32_t). Narrowing therefore only rejects values for types
// *narrower* than the slot (sub-32-bit ints, bool, sub-32-bit enums); a
// uint32_t/int32_t fills the slot, so every bit pattern is valid and there is
// no u64->narrower sign-extension or overflow to reject. The native mock
// (parse_mock) path -- including its handling of values that do not fit a
// 32-bit slot -- is covered in test_ffi/mock_syscall_test.cc.

// NOLINTBEGIN(bugprone-unchecked-optional-access)
TEST(SyscallParseTests, ParsePrimitiveTypes)
{
  AliasGuard guard;
  // some arbitrary initial data
  std::vector<uint8_t> memory = {0, 1, 2, 3, 4, 5, 6, 7};
  int128_t big_num = (int128_t{1} << 75) + 12;
  auto *addr = reinterpret_cast<uint8_t *>(&big_num);

  memory.insert(memory.end(), addr, addr + sizeof(big_num));

  auto contract_memory = std::as_writable_bytes(std::span(memory));

  // small input types ignore contract memory
  auto parsed_u32 =
      SyscallArgParams<uint32_t>::parse(guard, contract_memory, 1234);
  ASSERT_TRUE(parsed_u32.has_value());
  EXPECT_EQ(1234, parsed_u32.value());

  {
    // small output types work as regular pointers
    auto u32_output =
        SyscallArgParams<uint32_t &>::parse(guard, contract_memory, 2);
    ASSERT_TRUE(u32_output.has_value());

    u32_output.get() = 0x09080706;
  }
  EXPECT_EQ(memory[2], 0x06);
  EXPECT_EQ(memory[3], 0x07);
  EXPECT_EQ(memory[4], 0x08);
  EXPECT_EQ(memory[5], 0x09);

  // larger input types work as regular pointers
  auto int128_inp =
      SyscallArgParams<const int128_t &>::parse(guard, contract_memory, 8);
  ASSERT_TRUE(int128_inp);
  EXPECT_TRUE(SyscallArgParams<const int128_t &>::validate(int128_inp));
  // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
  EXPECT_EQ(big_num, *int128_inp);
}

TEST(SyscallParseTests, ParseMisalignedData)
{
  std::vector<uint8_t> memory = {15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5,
                                 4,  3,  2,  1,  0,  0,  0, 0, 0, 0, 0,
                                 0,  0,  0,  0,  0,  0,  0, 0, 0, 0};
  auto contract_memory = std::as_writable_bytes(std::span(memory));
  AliasGuard guard;

  auto check_int128 = [&](int128_t expect, size_t offset) {
    auto int128_inp =
        SyscallArgParams<const int128_t &>::parse(guard, contract_memory,
                                                  offset);
    ASSERT_TRUE(int128_inp);
    EXPECT_TRUE(SyscallArgParams<const int128_t &>::validate(int128_inp));
    EXPECT_EQ(expect, *int128_inp);
  };

  check_int128((int128_t{0x0001020304050607} << 64) +
                   int128_t{0x08090A0B0C0D0E0F},
               0);
  check_int128((int128_t{0x0000010203040506} << 64) +
                   int128_t{0x0708090A0B0C0D0E},
               1);
  check_int128((int128_t{0x0000000102030405} << 64) +
                   int128_t{0x060708090A0B0C0D},
               2);
  check_int128((int128_t{0x0000000001020304} << 64) +
                   int128_t{0x05060708090A0B0C},
               3);
  check_int128((int128_t{0x0000000000010203} << 64) +
                   int128_t{0x0405060708090A0B},
               4);
  check_int128((int128_t{0x0000000000000102} << 64) +
                   int128_t{0x030405060708090A},
               5);
  check_int128((int128_t{0x0000000000000001} << 64) +
                   int128_t{0x0203040506070809},
               6);
  check_int128((int128_t{0x0000000000000000} << 64) +
                   int128_t{0x0102030405060708},
               7);
  check_int128((int128_t{0x0000000000000000} << 64) +
                   int128_t{0x0001020304050607},
               8);
}

TEST(SyscallParseTests, ParseBoolRejectsNonCanonicalValues)
{
  std::vector<uint8_t> memory = {0, 1, 2, 3};
  auto contract_memory = std::as_writable_bytes(std::span(memory));
  AliasGuard guard;
  auto parsed_false = SyscallArgParams<bool>::parse(guard, contract_memory, 0);
  ASSERT_TRUE(parsed_false.has_value());
  EXPECT_FALSE(parsed_false.value());

  auto parsed_true = SyscallArgParams<bool>::parse(guard, contract_memory, 1);
  ASSERT_TRUE(parsed_true.has_value());
  EXPECT_TRUE(parsed_true.value());

  // Any value other than 0 or 1 in the slot is not a valid bool encoding.
  auto parsed_bad =
      SyscallArgParams<bool>::parse(guard, contract_memory,
                                    std::numeric_limits<uint32_t>::max());
  EXPECT_FALSE(parsed_bad.has_value());
  EXPECT_FALSE(SyscallArgParams<bool>::validate(parsed_bad));
}

TEST(SyscallParseTests, ParseEnumNarrowsViaUnderlyingType)
{
  std::vector<uint8_t> memory = {0, 1, 2, 3};
  auto contract_memory = std::as_writable_bytes(std::span(memory));
  AliasGuard guard;

  // A uint8_t-backed enum narrows like its underlying type: the value must fit
  // in 8 bits. 0xFF fits; 0x100 does not.
  auto parsed_u8_ok = SyscallArgParams<TestUnsignedEnum>::parse(
      guard, contract_memory, std::numeric_limits<uint8_t>::max());
  ASSERT_TRUE(parsed_u8_ok.has_value());
  EXPECT_EQ(parsed_u8_ok.value(), TestUnsignedEnum::kMax);

  auto parsed_u8_bad =
      SyscallArgParams<TestUnsignedEnum>::parse(guard, contract_memory, 0x100);
  EXPECT_FALSE(parsed_u8_bad.has_value());
  EXPECT_FALSE(SyscallArgParams<TestUnsignedEnum>::validate(parsed_u8_bad));

  // An int32_t-backed enum fills the 32-bit slot, so every bit pattern is a
  // valid (in-range) underlying value -- including ones that are negative when
  // interpreted as int32_t.
  auto parsed_i32_max = SyscallArgParams<TestSignedEnum>::parse(
      guard, contract_memory,
      static_cast<uint32_t>(std::numeric_limits<int32_t>::max()));
  ASSERT_TRUE(parsed_i32_max.has_value());

  auto parsed_i32_neg = SyscallArgParams<TestSignedEnum>::parse(
      guard, contract_memory, 0x80000000); // INT32_MIN bit pattern
  ASSERT_TRUE(parsed_i32_neg.has_value());
}

TEST(SyscallParseTests, OptionalPointerUsesExplicitNullSentinel)
{
  std::vector<uint8_t> memory = {1, 2, 3, 4, 5, 6, 7, 8};
  auto contract_memory = std::as_writable_bytes(std::span(memory));
  AliasGuard guard;

  auto valid_arg =
      SyscallArgParams<const uint32_t *>::parse(guard, contract_memory, 0);
  ASSERT_TRUE(SyscallArgParams<const uint32_t *>::validate(valid_arg));
  ASSERT_NE(SyscallArgParams<const uint32_t *>::deref(valid_arg), nullptr);

  // On the wasm32 path the null sentinel is the all-ones 32-bit slot value.
  auto null_arg = SyscallArgParams<const uint32_t *>::parse(
      guard, contract_memory, std::numeric_limits<uint32_t>::max());
  ASSERT_TRUE(SyscallArgParams<const uint32_t *>::validate(null_arg));
  ASSERT_EQ(SyscallArgParams<const uint32_t *>::deref(null_arg), nullptr);

  auto bad_arg =
      SyscallArgParams<const uint32_t *>::parse(guard, contract_memory, 16);
  ASSERT_FALSE(SyscallArgParams<const uint32_t *>::validate(bad_arg));
}

TEST(SyscallParseTests, WritableOutputBool)
{
  std::array<uint8_t, 8> memory = {0, 1, 2, 3, 4, 5, 6, 7};

  {
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    bool *val = reinterpret_cast<bool *>(&memory[3]);
    WritableOutput<bool> output(val);

    bool &ref = output.get();

    ref = true;
  }
  EXPECT_EQ(memory[3], 1);
  memory[3] = 3;
  EXPECT_EQ(memory, (std::array<uint8_t, 8>{0, 1, 2, 3, 4, 5, 6, 7}));

  {
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    bool *val = reinterpret_cast<bool *>(&memory[3]);
    WritableOutput<bool> output(val);

    bool &ref = output.get();

    ref = false;
  }
  EXPECT_EQ(memory[3], 0);
  memory[3] = 3;
  EXPECT_EQ(memory, (std::array<uint8_t, 8>{0, 1, 2, 3, 4, 5, 6, 7}));

  {
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    bool *val = reinterpret_cast<bool *>(&memory[3]);
    WritableOutput<bool> output(val);

    bool &ref = output.get();

    // NOLINTNEXTLINE(modernize-use-bool-literals,readability-implicit-bool-conversion)
    ref = 5;
  }
  EXPECT_EQ(memory[3], 1);
  memory[3] = 3;
  EXPECT_EQ(memory, (std::array<uint8_t, 8>{0, 1, 2, 3, 4, 5, 6, 7}));
}

// End-to-end over the real GetAuthClaim output path: a claim staged through
// WritableOutput lands in contract memory with its reserved bytes zeroed, even
// when the stack the value was built on was full of junk.  WritableOutput
// copies the whole object representation, so this is what the contract sees.
TEST(SyscallParseTests, WritableOutputWritesNoStackBytesForAuthClaim)
{
  constexpr size_t kSize = sizeof(generated::AuthClaim);
  std::array<uint8_t, kSize> memory{};
  memory.fill(0xFF);

  // Leave junk on the stack that the to_ffi call below will build its result
  // on.
  {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-member-init,hicpp-member-init)
    std::array<uint8_t, 1024> scratch;
    volatile uint8_t *scratch_bytes = scratch.data();
    for (size_t i = 0; i < scratch.size(); i++) {
      scratch_bytes[i] = 0xAB;
    }
  }

  AuthClaim claim(AuthClaimType::kPubkeySignature);
  claim.pubkey_preimage().pubkey.scheme(CryptographicScheme::Ed25519);
  claim.pubkey_preimage().pubkey.ed25519().fill(0x11);
  claim.pubkey_preimage().nonce = 0x2222222222222222ULL;

  {
    WritableOutput<generated::AuthClaim> output(memory.data());
    // Exactly what syscall_GetAuthClaim_inner does.
    output.get() = FFIHelperType<generated::AuthClaim>::to_ffi(claim);
  }

  EXPECT_EQ(memory[0], 1); // tag: kPubkeySignature
  for (size_t i = sizeof(generated::AuthClaim::Tag); i < 8; i++) {
    EXPECT_EQ(memory[i], 0) << "reserved byte " << i << " reached the contract";
  }
  // The payload itself survives.
  EXPECT_EQ(memory[8], 0); // nested PublicKey tag: Ed25519
  EXPECT_EQ(memory[12], 0x11);
  EXPECT_EQ(memory[43], 0x11);
  // The reserved gap before nonce.
  for (size_t i = 44; i < 48; i++) {
    EXPECT_EQ(memory[i], 0) << "reserved byte " << i << " reached the contract";
  }
  EXPECT_EQ(memory[48], 0x22);
}

// NOLINTEND(bugprone-unchecked-optional-access)

} // namespace hogd
