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

std::unique_ptr<StorageDeltaVec> dv;

auto const& get_dv() {
  return *dv;
}

auto const& get_deltas() {
  return dv -> deltas;
}


TEST(X23UnionTest, ShouldWorkFine)
{
  StorageDeltaVecList dvlist;

  Bytestring bstring;
  bstring.push_back(12);

  dv = std::make_unique<StorageDeltaVec>();
  dv->deltas.push_back(make_nnint64_set_add(100, 100));

  StorageDeltaVec vec2(get_dv().addr_key, get_deltas());

  dvlist.push_back(get_dv());

  dvlist.emplace_back(vec2.addr_key, vec2.deltas);

}

} // namespace hogd
