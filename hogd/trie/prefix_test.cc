#include <gtest/gtest.h>

#include "hogd/trie/prefix.h"

#include "hogd/trie/hashset_trie.h"

namespace hog {

TEST(Prefix, BranchbitsNull)
{
  using prefix_t = ByteArrayPrefix<2>;

  prefix_t prefix;
  EXPECT_EQ(prefix.get_branch_bits(0), 0);
}

TEST(Prefix, BranchbitsLong)
{
  using prefix_t = ByteArrayPrefix<16>;
  prefix_t prefix({0xFE, 0xDC, 0xBA, 0x98, 0x76, 0x54, 0x32, 0x10, 0xFE, 0xDC,
                   0xBA, 0x98, 0x76, 0x54, 0x32, 0xAB});

  EXPECT_EQ(prefix.get_branch_bits(0), 0xE);
  EXPECT_EQ(prefix.get_branch_bits(4), 0xF);
  EXPECT_EQ(prefix.get_branch_bits(16), 0xA);
  EXPECT_EQ(prefix.get_branch_bits(64), 0xE);
  EXPECT_EQ(prefix.get_branch_bits(120), 0xB);
  EXPECT_EQ(prefix.get_branch_bits(124), 0xA);
}

TEST(Prefix, Truncate)
{
  using prefix_t = ByteArrayPrefix<16>;
  prefix_t prefix({0xFE, 0xDC, 0xBA, 0x98, 0x76, 0x54, 0x32, 0x10, 0xFE, 0xDC,
                   0xBA, 0x98, 0x76, 0x54, 0x32, 0x01});
  prefix_t prefix_clone = prefix;

  // truncate at 120
  prefix_t prefix2({0xFE, 0xDC, 0xBA, 0x98, 0x76, 0x54, 0x32, 0x10, 0xFE, 0xDC,
                    0xBA, 0x98, 0x76, 0x54, 0x32, 0x00});
  // truncate at 116
  prefix_t prefix3({0xFE, 0xDC, 0xBA, 0x98, 0x76, 0x54, 0x32, 0x10, 0xFE, 0xDC,
                    0xBA, 0x98, 0x76, 0x54, 0x02, 0x00});
  // truncate at 44
  prefix_t prefix4({0xFE, 0xDC, 0xBA, 0x98, 0x76, 0x04, 0x00, 0x00, 0x00, 0x00,
                    0x00, 0x00, 0x00, 0x00, 0x00, 0x00});

  EXPECT_EQ(prefix, prefix_clone);

  prefix_clone.truncate(128);

  EXPECT_EQ(prefix, prefix_clone);

  prefix.truncate(124);
  // because last branch_bits is 0
  EXPECT_EQ(prefix, prefix_clone);

  prefix.truncate(120);
  EXPECT_EQ(prefix, prefix2);

  prefix.truncate(116);
  EXPECT_EQ(prefix, prefix3);

  prefix.truncate(44);
  EXPECT_EQ(prefix, prefix4);
}

TEST(HashsetPrefix, Branchbits)
{
  const auto prefix =
      HashsetTrieNode::make_prefix(0xABCD'1234'EF01'5678, Hash());
  EXPECT_EQ(prefix.get_branch_bits(0), 0xA);
  EXPECT_EQ(prefix.get_branch_bits(4), 0xB);
  EXPECT_EQ(prefix.get_branch_bits(8), 0xC);
  EXPECT_EQ(prefix.get_branch_bits(12), 0xD);
  EXPECT_EQ(prefix.get_branch_bits(48), 0x5);
  EXPECT_EQ(prefix.get_branch_bits(52), 0x6);
  EXPECT_EQ(prefix.get_branch_bits(56), 0x7);
  EXPECT_EQ(prefix.get_branch_bits(60), 0x8);
}

} // namespace hog
