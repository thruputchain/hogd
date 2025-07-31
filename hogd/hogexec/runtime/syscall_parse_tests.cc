#include <gtest/gtest.h>

#include <span>
#include <vector>

#include "hogd/base/types.h"
#include "hogd/hogexec/runtime/syscall_arg_params.h"

namespace hogd {

TEST(SyscallParseTests, ParsePrimitiveTypes)
{
  // some arbitrary iniital data
  std::vector<uint8_t> memory = {0, 1, 2, 3, 4, 5, 6, 7};
  int128_t big_num = (int128_t{1} << 75) + 12;
  auto *addr = reinterpret_cast<uint8_t *>(&big_num);

  memory.insert(memory.end(), addr, addr + sizeof(big_num));

  auto contract_memory = std::as_writable_bytes(std::span(memory));

  // small input types ignore contract memory
  EXPECT_EQ(1234, SyscallArgParams<uint32_t>::parse(contract_memory, 1234));

  {
    // small output types work as regular pointers
    auto u32_output = SyscallArgParams<uint32_t &>::parse(contract_memory, 2);
    ASSERT_TRUE(u32_output.has_value());

    u32_output.get() = 0x09080706;
  }
  EXPECT_EQ(memory[2], 0x06);
  EXPECT_EQ(memory[3], 0x07);
  EXPECT_EQ(memory[4], 0x08);
  EXPECT_EQ(memory[5], 0x09);

  // larger input types work as regular pointers
  auto int128_inp = SyscallArgParams<int128_t>::parse(contract_memory, 8);
  ASSERT_TRUE(int128_inp);
  EXPECT_TRUE(SyscallArgParams<int128_t>::validate(int128_inp));
  // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
  EXPECT_EQ(big_num, *int128_inp);
}

TEST(SyscallParseTests, ParseMisalignedData)
{
  std::vector<uint8_t> memory = { 15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
  auto contract_memory = std::as_writable_bytes(std::span(memory));

  auto check_int128 = [&](int128_t expect, size_t offset) {
    auto int128_inp = SyscallArgParams<int128_t>::parse(contract_memory, offset);
    ASSERT_TRUE(int128_inp);
    EXPECT_TRUE(SyscallArgParams<int128_t>::validate(int128_inp));
    EXPECT_EQ(expect, *int128_inp);
  };

  check_int128((int128_t{0x0001020304050607} << 64) + int128_t{0x08090A0B0C0D0E0F}, 0);
  check_int128((int128_t{0x0000010203040506} << 64) + int128_t{0x0708090A0B0C0D0E}, 1);
  check_int128((int128_t{0x0000000102030405} << 64) + int128_t{0x060708090A0B0C0D}, 2);
  check_int128((int128_t{0x0000000001020304} << 64) + int128_t{0x05060708090A0B0C}, 3);
  check_int128((int128_t{0x0000000000010203} << 64) + int128_t{0x0405060708090A0B}, 4);
  check_int128((int128_t{0x0000000000000102} << 64) + int128_t{0x030405060708090A}, 5);
  check_int128((int128_t{0x0000000000000001} << 64) + int128_t{0x0203040506070809}, 6);
  check_int128((int128_t{0x0000000000000000} << 64) + int128_t{0x0102030405060708}, 7);
  check_int128((int128_t{0x0000000000000000} << 64) + int128_t{0x0001020304050607}, 8);
}

} // namespace hogd
