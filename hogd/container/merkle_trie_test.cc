#include <gtest/gtest.h>

#include <atomic>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "hogd/container/merkle_trie.h"

namespace hogd {

TEST(MerkleTrie, BasicUsage)
{
  std::cout << "Hello world! This is the Merkle Trie test." << std::endl;
}

} // namespace hogd

int
main(int argc, char *argv[])
{
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
