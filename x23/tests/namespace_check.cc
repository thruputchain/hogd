
#include <cassert>

#include "tests/xdr_namespace_test2.hh"
// Declares get<I> overloads in namespace x23, which hide any at global scope
// from unqualified lookup inside x23.
#include "x23/rpc_msg.hh" // IWYU pragma: keep
#include "x23/serialize.h"

// keep to ensure that this file compiles
#include "tests/xdr_namespace_test3.hh" // IWYU pragma: keep

using namespace xdr_namespace_test_2;

int
main()
{
  [[maybe_unused]] c check{};
  [[maybe_unused]] xdr_namespace_test_1::ns_1_struct check2{};

  // A boxed arm of a namespaced struct type round-trips.
  using namespace xdr_namespace_test_1;
  ns_1_boxed boxed(ns_1_boxed_type::kNs1Inner);
  boxed.inner()->a = 7;
  boxed.inner()->b = {1, 2, 3};
  x23::Buf buf = x23::serialize(boxed);
  ns_1_boxed back;
  x23::deserialize(&back, buf);
  assert(back == boxed);
  return 0;
}
