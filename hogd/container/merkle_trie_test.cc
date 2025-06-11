#include <gtest/gtest.h>
#include <string.h>

#include <atomic>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "hogd/container/merkle_trie.h"

namespace hogd {

namespace {

// Converts a human-readable hex string (e.g., "0x1234") to a [SHA256Hex].
SHA256Hash
HashFromString(const std::string &str)
{
  // A SHA-256 hash is 32 bytes, so [str] should have 32 * 2 = 64
  // characters.
  assert(str.size() == 64);

  SHA256Hash ret;
  for (size_t i = 0; i < str.size(); i += 2) {
    std::string substr = str.substr(i, 2);
    unsigned char byte = static_cast<unsigned char>(
        std::stoul(substr, /*pos=*/nullptr, /*base=*/16));
    ret[i / 2] = byte;
  }
  return ret;
}

} // namespace

TEST(MerkleTrie, BasicUsage)
{
  std::cout << "Hello world! This is the Merkle Trie test." << std::endl;
  MerkleTrie trie;
}

TEST(MerkleTrie, HashLeaf)
{
  MerkleTrie::Node leaf;
  leaf.type = MerkleTrie::NodeType::kLeaf;
  memset(leaf.hash.data(), 0, leaf.hash.size() * sizeof(leaf.hash[0]));
  leaf.leaf_path = {std::byte{0x00}, std::byte{0x01}, std::byte{0x02},
                    std::byte{0x03}};
  leaf.leaf_value = "hello";

  SHA256Hash hash = MerkleTrie::GetNodeHash(leaf);
  SHA256Hash expected = HashFromString(
      "f5c843c64f025c69d3abefe86036c1288bf825ca97a52b0f0aab2e422a49a036");
  EXPECT_EQ(hash, expected);
}

} // namespace hogd

int
main(int argc, char *argv[])
{
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
