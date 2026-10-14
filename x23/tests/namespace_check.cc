
#include "tests/xdr_namespace_test2.hh"

// keep to ensure that this file compiles
#include "tests/xdr_namespace_test3.hh" // IWYU pragma: keep

using namespace xdr_namespace_test_2;

int
main()
{
  [[maybe_unused]] c check{};
  [[maybe_unused]] xdr_namespace_test_1::ns_1_struct check2{};
  return 0;
}
