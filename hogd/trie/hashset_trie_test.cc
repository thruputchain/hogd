#include <gtest/gtest.h>

#include "hogd/trie/hashset_trie.h"

namespace hog {

TEST(HashsetTrie, SeveralInserts)
{
  HashsetTrie root;

  EXPECT_TRUE(root.insert(20, Hash()));
  EXPECT_TRUE(root.insert(30, Hash()));
  EXPECT_TRUE(root.insert(50, Hash()));
  EXPECT_TRUE(root.insert(32, Hash()));
  EXPECT_TRUE(root.insert(15, Hash()));
  EXPECT_TRUE(root.insert(0xAAAA, Hash()));
  EXPECT_TRUE(root.insert(0xAAAB, Hash()));
  EXPECT_TRUE(root.insert(0xAADB, Hash()));

  EXPECT_FALSE(root.insert(20, Hash()));
}

TEST(HashsetTrie, Removals)
{
  HashsetTrie root;

  EXPECT_TRUE(root.insert(0xAAAA, Hash()));
  EXPECT_TRUE(root.insert(0xAAAB, Hash()));
  EXPECT_TRUE(root.insert(0xAAAC, Hash()));
  EXPECT_TRUE(root.insert(0xAABA, Hash()));
  EXPECT_TRUE(root.insert(0xAABB, Hash()));
  EXPECT_TRUE(root.insert(0xAACA, Hash()));
  EXPECT_TRUE(root.insert(0x0000, Hash()));
  EXPECT_TRUE(root.insert(0xBAAA, Hash()));

  auto res = root.remove_clear_threshold(0xAAC0);

  EXPECT_EQ(res.size(), 3);

  // check removals went appropriately
  EXPECT_TRUE(root.insert(0xAAAA, Hash()));
  EXPECT_TRUE(root.insert(0xAAAB, Hash()));
  EXPECT_TRUE(root.insert(0xAAAC, Hash()));
  EXPECT_TRUE(root.insert(0xAABA, Hash()));
  EXPECT_TRUE(root.insert(0xAABB, Hash()));
  EXPECT_TRUE(root.insert(0x0000, Hash()));

  EXPECT_FALSE(root.insert(0xAACA, Hash()));
}

TEST(HashsetTrie, RemovalOnMargin)
{
  HashsetTrie root;

  root.insert(0xAAAA, Hash());
  root.insert(0xAAAB, Hash());
  root.insert(0xAAAC, Hash());
  root.insert(0xAABA, Hash());
  root.insert(0xAABB, Hash());
  root.insert(0xAAC0, Hash());
  root.insert(0x0000, Hash());
  root.insert(0xBAAA, Hash());

  auto res = root.remove_clear_threshold(0xAAC0);

  EXPECT_EQ(res.size(), 4);

  // check removals went appropriately
  root.insert(0xAAAA, Hash());
  root.insert(0xAAAB, Hash());
  root.insert(0xAAAC, Hash());
  root.insert(0xAABA, Hash());
  root.insert(0xAABB, Hash());
  root.insert(0x0000, Hash());

  root.insert(0xAACA, Hash());
  root.insert(0xAAC0, Hash());
}

TEST(HashsetTrie, RemovalOnMargin2)
{
  HashsetTrie root;

  Hash extra;
  extra[15] = 12;

  root.insert(0xAAAA, Hash());
  root.insert(0xAAAB, Hash());
  root.insert(0xAAAC, Hash());
  root.insert(0xAABA, Hash());
  root.insert(0xAABB, Hash());
  root.insert(0xAAC0, Hash());
  root.insert(0xAAC0, extra);
  root.insert(0x0000, Hash());
  root.insert(0xBAAA, Hash());

  auto res = root.remove_clear_threshold(0xAAC0);

  EXPECT_EQ(res.size(), 4);

  // check removals went appropriately
  root.insert(0xAAAA, Hash());
  root.insert(0xAAAB, Hash());
  root.insert(0xAAAC, Hash());
  root.insert(0xAABA, Hash());
  root.insert(0xAABB, Hash());
  root.insert(0x0000, Hash());

  root.insert(0xAACA, Hash());
  root.insert(0xAAC0, Hash());
  root.insert(0xAAC0, extra);
}

} // namespace hog
