#include <gtest/gtest.h>

#include <cstddef>
#include <cstdlib>
#include <cstring>

#include "hogd/base/status.h"
#include "hogd/base/types.h"
#include "hogd/init.h"
#include "hogd/softstate/hashset_deduplicate.h"
#include "types.hh"

namespace hogd {

TEST(Hashset, SimpleDeDuplicate)
{
  HashsetDeduplicatorSimple deduplicator;

  auto make_entry = [](uint64_t value) -> SHA256Hash {
    SHA256Hash out;
    std::memcpy(out.data(), &value, sizeof(uint64_t));
    return out;
  };

  EXPECT_TRUE(deduplicator.try_insert(make_entry(0)));
  EXPECT_TRUE(deduplicator.try_insert(make_entry(1)));
  EXPECT_TRUE(deduplicator.try_insert(make_entry(2)));

  EXPECT_FALSE(deduplicator.try_insert(make_entry(0)));
  EXPECT_FALSE(deduplicator.try_insert(make_entry(1)));
  EXPECT_FALSE(deduplicator.try_insert(make_entry(2)));

  deduplicator.remove(make_entry(0));

  EXPECT_TRUE(deduplicator.try_insert(make_entry(0)));
}

TEST(Hashset, SimpleDeDuplicate2)
{
  HashsetDeduplicatorSimpleV2 deduplicator;

  auto make_entry = [](uint64_t value) -> SHA256Hash {
    SHA256Hash out;
    std::memcpy(out.data(), &value, sizeof(uint64_t));
    return out;
  };

  EXPECT_TRUE(deduplicator.try_insert(make_entry(0)));
  EXPECT_TRUE(deduplicator.try_insert(make_entry(1)));
  EXPECT_TRUE(deduplicator.try_insert(make_entry(2)));

  EXPECT_FALSE(deduplicator.try_insert(make_entry(0)));
  EXPECT_FALSE(deduplicator.try_insert(make_entry(1)));
  EXPECT_FALSE(deduplicator.try_insert(make_entry(2)));

  deduplicator.remove(make_entry(0));

  EXPECT_TRUE(deduplicator.try_insert(make_entry(0)));
}

TEST(Hashset, AtomicDeduplicate)
{
  AtomicHashsetDeduplicator deduplicator;

  auto make_entry = [](uint64_t value) -> SHA256Hash {
    SHA256Hash out;
    std::memcpy(out.data(), &value, sizeof(uint64_t));
    return out;
  };

  EXPECT_TRUE(deduplicator.try_insert(make_entry(0)));
  EXPECT_TRUE(deduplicator.try_insert(make_entry(1)));
  EXPECT_TRUE(deduplicator.try_insert(make_entry(2)));

  EXPECT_FALSE(deduplicator.try_insert(make_entry(0)));
  EXPECT_FALSE(deduplicator.try_insert(make_entry(1)));
  EXPECT_FALSE(deduplicator.try_insert(make_entry(2)));

  deduplicator.remove(make_entry(0));

  EXPECT_TRUE(deduplicator.try_insert(make_entry(0)));
}

} // namespace hogd

int
main(int argc, char *argv[])
{
  using hogd::GStatus;
  GStatus<void> ret = hogd::crypto_init();
  if (!ret)
    return EXIT_FAILURE;
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
