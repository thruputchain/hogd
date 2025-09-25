#include <gtest/gtest.h>

#include "hogd/test_ffi/test_bindings.h"

namespace hogd {

// Simple tests to make sure that the exported test functions work on at least a
// basic level. This one, for example, catches an error in initializing hogd.
TEST(TestBindingsTest, LazyInitTest)
{
  auto *db = create_db();
  ASSERT_NE(db, nullptr);
  free_db(db);
}

} // namespace hogd
