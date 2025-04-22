#include <gtest/gtest.h>

#include <cstddef>
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

  // small output types work as regular pointers
  uint32_t *u32_output =
      SyscallArgParams<uint32_t &>::parse(contract_memory, 2);
  ASSERT_TRUE(u32_output);
  EXPECT_EQ(*u32_output, 0x05040302);

  *u32_output = 0;
  EXPECT_EQ(memory[2], 0);
  EXPECT_EQ(memory[3], 0);
  EXPECT_EQ(memory[4], 0);
  EXPECT_EQ(memory[5], 0);

  // larger input types work as regular pointers
  auto *int128_inp = SyscallArgParams<int128_t>::parse(contract_memory, 8);
  ASSERT_TRUE(int128_inp);
  EXPECT_TRUE(SyscallArgParams<int128_t>::validate(int128_inp));
  EXPECT_EQ(big_num, *int128_inp);
}

} // namespace hogd
