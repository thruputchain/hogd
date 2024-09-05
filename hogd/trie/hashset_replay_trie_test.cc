#include <gtest/gtest.h>

#include "hogd/trie/hashset_replay_trie.h"
#include "hogd/trie/hashset_replay_trie_node.h"
#include "hogd/trie/short_allocator.h"

namespace hog {

using allocator_t = ShortAllocator<ReplayTrieNode>;

TEST(HashsetReplayTrie, DoubleInsert)
{
  allocator_t allocator;
  ReplayTrie replay_tree(allocator);
  auto insert_ref = replay_tree.open_for_insertions();

  EXPECT_TRUE(insert_ref.try_insert({0, 0, 0, 0}));
  EXPECT_FALSE(insert_ref.try_insert({0, 0, 0, 0}));

  EXPECT_TRUE(insert_ref.try_insert({1, 0, 0, 0}));
  EXPECT_TRUE(insert_ref.try_insert({2, 0, 0, 0}));
  EXPECT_TRUE(insert_ref.try_insert({3, 0, 0, 0}));

  EXPECT_FALSE(insert_ref.try_insert({1, 0, 0, 0}));
  EXPECT_FALSE(insert_ref.try_insert({2, 0, 0, 0}));
  EXPECT_FALSE(insert_ref.try_insert({3, 0, 0, 0}));
}

TEST(HashsetReplayTrie, InsertRemoveInsert)
{
  allocator_t allocator;
  ReplayTrie replay_tree(allocator);

  {
    auto insert_ref = replay_tree.open_for_insertions();

    EXPECT_TRUE(insert_ref.try_insert({0, 0, 0, 0}));
    EXPECT_FALSE(insert_ref.try_insert({0, 0, 0, 0}));

    EXPECT_TRUE(insert_ref.try_insert({1, 0, 0, 0}));
    EXPECT_TRUE(insert_ref.try_insert({2, 0, 0, 0}));
    EXPECT_TRUE(insert_ref.try_insert({3, 0, 0, 0}));
    EXPECT_TRUE(insert_ref.try_insert({4, 0, 0, 0}));
    EXPECT_TRUE(insert_ref.try_insert({5, 0, 0, 0}));
    EXPECT_TRUE(insert_ref.try_insert({6, 0, 0, 0}));
    EXPECT_TRUE(insert_ref.try_insert({7, 0, 0, 0}));
    EXPECT_TRUE(insert_ref.try_insert({8, 0, 0, 0}));
    EXPECT_TRUE(insert_ref.try_insert({9, 0, 0, 0}));
  }

  {
    auto remove_ref = replay_tree.open_for_removals();

    remove_ref.remove({0, 0, 0, 0});

    // EXPECT_ANY_THROW(remove_ref.remove({0, 1, 2, 3}));

    remove_ref.remove({2, 0, 0, 0});

    // EXPECT_ANY_THROW(remove_ref.remove({2, 0, 0, 0}));

    remove_ref.remove({4, 0, 0, 0});
    remove_ref.remove({6, 0, 0, 0});
  }

  {
    auto insert_ref = replay_tree.open_for_insertions();

    EXPECT_TRUE(insert_ref.try_insert({0, 0, 0, 0}));
    EXPECT_TRUE(insert_ref.try_insert({2, 0, 0, 0}));
    EXPECT_TRUE(insert_ref.try_insert({4, 0, 0, 0}));
    EXPECT_TRUE(insert_ref.try_insert({6, 0, 0, 0}));

    EXPECT_FALSE(insert_ref.try_insert({0, 0, 0, 0}));
    EXPECT_FALSE(insert_ref.try_insert({1, 0, 0, 0}));
    EXPECT_FALSE(insert_ref.try_insert({2, 0, 0, 0}));
    EXPECT_FALSE(insert_ref.try_insert({3, 0, 0, 0}));
    EXPECT_FALSE(insert_ref.try_insert({4, 0, 0, 0}));
  }
}

TEST(HashsetReplayTrie, CompleteClear)
{
  allocator_t allocator;
  ReplayTrie replay_tree(allocator);

  {
    auto insert_ref = replay_tree.open_for_insertions();

    EXPECT_TRUE(insert_ref.try_insert({0, 0, 0, 0}));
    EXPECT_FALSE(insert_ref.try_insert({0, 0, 0, 0}));

    EXPECT_TRUE(insert_ref.try_insert({1, 0, 0, 0}));
    EXPECT_TRUE(insert_ref.try_insert({2, 0, 0, 0}));
    EXPECT_TRUE(insert_ref.try_insert({3, 0, 0, 0}));
    EXPECT_TRUE(insert_ref.try_insert({4, 0, 0, 0}));
    EXPECT_TRUE(insert_ref.try_insert({5, 0, 0, 0}));
    EXPECT_TRUE(insert_ref.try_insert({6, 0, 0, 0}));
    EXPECT_TRUE(insert_ref.try_insert({7, 0, 0, 0}));
    EXPECT_TRUE(insert_ref.try_insert({8, 0, 0, 0}));
    EXPECT_TRUE(insert_ref.try_insert({9, 0, 0, 0}));
  }

  {
    auto remove_ref = replay_tree.open_for_removals();

    remove_ref.remove({0, 0, 0, 0});
    remove_ref.remove({1, 0, 0, 0});
    remove_ref.remove({2, 0, 0, 0});
    remove_ref.remove({3, 0, 0, 0});
    remove_ref.remove({4, 0, 0, 0});
    remove_ref.remove({5, 0, 0, 0});
    remove_ref.remove({6, 0, 0, 0});
    remove_ref.remove({7, 0, 0, 0});
    remove_ref.remove({8, 0, 0, 0});
    remove_ref.remove({9, 0, 0, 0});
  }
}

TEST(HashsetReplayTrie, UndoInsert)
{
  allocator_t allocator;
  ReplayTrie replay_tree(allocator);

  {
    auto insert_ref = replay_tree.open_for_insertions();

    EXPECT_TRUE(insert_ref.try_insert({0, 0, 0, 0}));
    EXPECT_FALSE(insert_ref.try_insert({0, 0, 0, 0}));

    EXPECT_TRUE(insert_ref.try_insert({1, 0, 0, 0}));
    EXPECT_TRUE(insert_ref.try_insert({2, 0, 0, 0}));
    EXPECT_TRUE(insert_ref.try_insert({3, 0, 0, 0}));

    insert_ref.undo_try_insert({0, 0, 0, 0});

    EXPECT_TRUE(insert_ref.try_insert({0, 0, 0, 0}));

    insert_ref.undo_try_insert({0, 0, 0, 0});
    insert_ref.undo_try_insert({1, 0, 0, 0});

    //	EXPECT_ANY_THROW(insert_ref.undo_try_insert({1, 0, 0, 0}));
  }

  {
    auto remove_ref = replay_tree.open_for_removals();

    // EXPECT_ANY_THROW(remove_ref.remove({0, 0, 0, 0}));

    remove_ref.gc_if_empty({0, 0, 0, 0});
    // EXPECT_ANY_THROW(remove_ref.gc_if_empty({0, 0, 0, 0}));

    remove_ref.gc_if_empty({2, 0, 0, 0});
  }
}

} // namespace hog
