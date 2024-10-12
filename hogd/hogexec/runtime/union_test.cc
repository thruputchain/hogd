#include <gtest/gtest.h>

#include "storage_object.hh"

namespace hogd {

namespace test {

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
std::unique_ptr<StorageDeltaVec> dv;

auto const& get_dv() {
  return *dv;
}

auto const& get_deltas() {
  return dv -> deltas;
}


TEST(X23UnionTest, ShouldWorkFine)
{

  StorageDeltaVec dv;

  dv.deltas.emplace_back();

  StorageDelta d = test::make_nnint64_set_add(100, 10);

  dv.deltas.back() = d;

  EXPECT_EQ(d.type(), dv.deltas.back().type());

}

} // namespace hogd
