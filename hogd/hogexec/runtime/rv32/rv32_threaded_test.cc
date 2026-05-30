#include <gtest/gtest.h>

#include "hogd/hogexec/runtime/rv32/musttail.h"

namespace hogd::rv32 {
namespace {

// Smoke test: the threaded target builds and links. Real tests arrive in later
// tasks. Verifies HOGD_MUSTTAIL is defined (compiles) and a tail-call lowers.
[[nodiscard]] int
tail_leaf(int x)
{
  return x + 1;
}
[[nodiscard]] int
tail_caller(int x)
{
  HOGD_MUSTTAIL return tail_leaf(x);
}

TEST(Rv32Threaded, harnessBuildsAndMusttailLowers)
{
  EXPECT_EQ(tail_caller(41), 42);
}

} // namespace
} // namespace hogd::rv32
