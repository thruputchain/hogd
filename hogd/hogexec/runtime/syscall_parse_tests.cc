#include <gtest/gtest.h>

#include <array>
#include <limits>
#include <span>
#include <vector>

#include "hogd/base/types.h"
#include "hogd/hogexec/components/alias_guard.h"
#include "hogd/hogexec/runtime/syscall_arg_params.h"
#include "hogd/hogexec/runtime/syscall_argument_parsers.h"

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

// NOLINTBEGIN(bugprone-unchecked-optional-access)
TEST(SyscallParseTests, ParsePrimitiveTypes)
{
  AliasGuard guard;
  // some arbitrary iniital data
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

TEST(SyscallParseTests, ParseUnsignedNarrowRejectsOverflow)
{
  AliasGuard guard;
  std::vector<uint8_t> memory = {0, 1, 2, 3};
  auto contract_memory = std::as_writable_bytes(std::span(memory));

  auto parsed_u32 = SyscallArgParams<uint32_t>::parse(guard, contract_memory,
                                                      uint64_t{1} << 32);
  EXPECT_FALSE(parsed_u32.has_value());
  EXPECT_FALSE(SyscallArgParams<uint32_t>::validate(parsed_u32));
}

TEST(SyscallParseTests, ParseSignedNarrowRejectsNonSignExtended)
{
  AliasGuard guard;
  std::vector<uint8_t> memory = {0, 1, 2, 3};
  auto contract_memory = std::as_writable_bytes(std::span(memory));

  // Sign-extended -1 is valid.
  auto parsed_neg1 = SyscallArgParams<int32_t>::parse(guard, contract_memory,
                                                      0xFFFFFFFFFFFFFFFFULL);
  ASSERT_TRUE(parsed_neg1.has_value());
  EXPECT_EQ(*parsed_neg1, -1);

  // Sign-extended INT32_MIN is valid.
  auto parsed_min = SyscallArgParams<int32_t>::parse(guard, contract_memory,
                                                     0xFFFFFFFF80000000ULL);
  ASSERT_TRUE(parsed_min.has_value());
  EXPECT_EQ(*parsed_min, std::numeric_limits<int32_t>::min());

  // INT32_MAX is valid.
  auto parsed_max = SyscallArgParams<int32_t>::parse(guard, contract_memory,
                                                     0x000000007FFFFFFFULL);
  ASSERT_TRUE(parsed_max.has_value());
  EXPECT_EQ(*parsed_max, std::numeric_limits<int32_t>::max());

  // Zero is valid.
  auto parsed_zero =
      SyscallArgParams<int32_t>::parse(guard, contract_memory, 0);
  ASSERT_TRUE(parsed_zero.has_value());
  EXPECT_EQ(*parsed_zero, 0);

  // Non-sign-extended -1 is rejected (0x00000000FFFFFFFF).
  auto parsed_bad = SyscallArgParams<int32_t>::parse(guard, contract_memory,
                                                     0x00000000FFFFFFFFULL);
  EXPECT_FALSE(parsed_bad.has_value());
  EXPECT_FALSE(SyscallArgParams<int32_t>::validate(parsed_bad));

  // INT32_MAX + 1 (0x0000000080000000) is rejected.
  auto parsed_overflow =
      SyscallArgParams<int32_t>::parse(guard, contract_memory,
                                       0x0000000080000000ULL);
  EXPECT_FALSE(parsed_overflow.has_value());

  // Below INT32_MIN is rejected.
  auto parsed_underflow =
      SyscallArgParams<int32_t>::parse(guard, contract_memory,
                                       0xFFFFFFFF7FFFFFFFULL);
  EXPECT_FALSE(parsed_underflow.has_value());
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

  auto parsed_bad =
      SyscallArgParams<bool>::parse(guard, contract_memory,
                                    std::numeric_limits<uint64_t>::max());
  EXPECT_FALSE(parsed_bad.has_value());
  EXPECT_FALSE(SyscallArgParams<bool>::validate(parsed_bad));
}

TEST(SyscallParseTests, ParseEnumRejectsOutOfRangeUnderlyingValues)
{
  std::vector<uint8_t> memory = {0, 1, 2, 3};
  auto contract_memory = std::as_writable_bytes(std::span(memory));
  AliasGuard guard;
  auto parsed_u8_ok = SyscallArgParams<TestUnsignedEnum>::parse(
      guard, contract_memory, std::numeric_limits<uint8_t>::max());
  ASSERT_TRUE(parsed_u8_ok.has_value());
  EXPECT_EQ(parsed_u8_ok.value(), TestUnsignedEnum::kMax);

  auto parsed_u8_bad =
      SyscallArgParams<TestUnsignedEnum>::parse(guard, contract_memory, 0x100);
  EXPECT_FALSE(parsed_u8_bad.has_value());
  EXPECT_FALSE(SyscallArgParams<TestUnsignedEnum>::validate(parsed_u8_bad));

  auto parsed_i32_ok = SyscallArgParams<TestSignedEnum>::parse(
      guard, contract_memory,
      static_cast<uint64_t>(std::numeric_limits<int32_t>::max()));
  ASSERT_TRUE(parsed_i32_ok.has_value());

  auto parsed_i32_bad = SyscallArgParams<TestSignedEnum>::parse(
      guard, contract_memory,
      static_cast<uint64_t>(std::numeric_limits<int32_t>::max()) + 1);
  EXPECT_FALSE(parsed_i32_bad.has_value());
  EXPECT_FALSE(SyscallArgParams<TestSignedEnum>::validate(parsed_i32_bad));

  int64_t below_min_i32 =
      static_cast<int64_t>(std::numeric_limits<int32_t>::min()) - 1;
  auto parsed_i32_bad_low = SyscallArgParams<TestSignedEnum>::parse(
      guard, contract_memory, static_cast<uint64_t>(below_min_i32));
  EXPECT_FALSE(parsed_i32_bad_low.has_value());
  EXPECT_FALSE(SyscallArgParams<TestSignedEnum>::validate(parsed_i32_bad_low));

  auto parsed_i32_low_ok = SyscallArgParams<TestSignedEnum>::parse(
      guard, contract_memory,
      static_cast<uint64_t>(std::numeric_limits<int32_t>::min()));
  ASSERT_TRUE(parsed_i32_low_ok.has_value());

  // Non-sign-extended negative encodings are rejected.
  auto parsed_i32_bad_nonsignext =
      SyscallArgParams<TestSignedEnum>::parse(guard, contract_memory,
                                              0x00000000FFFFFFFFULL);
  EXPECT_FALSE(parsed_i32_bad_nonsignext.has_value());
  EXPECT_FALSE(
      SyscallArgParams<TestSignedEnum>::validate(parsed_i32_bad_nonsignext));

  auto parsed_i32_low_ok_mock = SyscallArgParams<TestSignedEnum>::parse_mock(
      guard, static_cast<uint64_t>(std::numeric_limits<int32_t>::min()));
  ASSERT_TRUE(parsed_i32_low_ok_mock.has_value());

  auto parsed_i32_bad_mock = SyscallArgParams<TestSignedEnum>::parse_mock(
      guard, static_cast<uint64_t>(std::numeric_limits<int32_t>::max()) + 1);
  EXPECT_FALSE(parsed_i32_bad_mock.has_value());

  auto parsed_i32_sign_extended_neg1_mock =
      SyscallArgParams<TestSignedEnum>::parse_mock(
          guard, std::numeric_limits<uint64_t>::max());
  ASSERT_TRUE(parsed_i32_sign_extended_neg1_mock.has_value());
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

  auto null_arg = SyscallArgParams<const uint32_t *>::parse(
      guard, contract_memory, std::numeric_limits<uint64_t>::max());
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

// NOLINTEND(bugprone-unchecked-optional-access)

} // namespace hogd
