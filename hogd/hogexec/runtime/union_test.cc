#include <gtest/gtest.h>

#include "storage_object.hh"

namespace hogd {

namespace {

[[maybe_unused]]
StorageDelta
make_bytestring_set(Bytestring const &string)
{
  StorageDelta out;
  out.type(StorageDeltaType::kBytestringSet);
  out.bytestring() = string;
  return out;
}

StorageDelta
make_nnint64_set_add(int64_t set, int64_t add)
{
  StorageDelta out;
  out.type(StorageDeltaType::kNonnegativeInt64SetAdd);
  out.nnint64() = NonnegativeInt64SetAddArgs{set, add};
  return out;
} 

}

TEST(X23UnionTest, ShouldWorkFine)
{

  StorageDeltaVecList dvlist;

  Bytestring bstring;
  bstring.push_back(12);

  StorageDeltaVec vec;
  vec.deltas.push_back(make_nnint64_set_add(100, 100));

  StorageDeltaVec vec2(vec.addr_key, vec.deltas);

  dvlist.push_back(vec);

  dvlist.emplace_back(vec.addr_key, vec.deltas);

}

} // namespace hogd
