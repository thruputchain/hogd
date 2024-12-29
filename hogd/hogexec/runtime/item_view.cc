#include "hogd/hogexec/runtime/item_view.h"

#include <cassert>
#include <concepts>
#include <cstddef>
#include <limits>
#include <optional>
#include <span>
#include <type_traits>
#include <utility>
#include <variant>

#include "hogd/base/compiler.h"
#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h"
#include "hogd/hogexec/runtime/status.h"
#include "storage_object.hh"
#include "syscall.hh"

namespace hogd {

namespace {

StorageDelta
make_bytestring_set(Bytestring const &string)
{
  StorageDelta out;
  out.type(StorageDeltaType::kBytestringSet);
  out.bytestring() = string;
  return out;
}

StorageDelta
make_nnint128_set_add(int128_t set, int128_t add)
{
  StorageDelta out(StorageDeltaType::kNonnegativeIntSetAdd);
  out.nnint() = NonnegativeInt128SetAddArgs{.set_value = to_xdr(set),
                                            .add_value = to_xdr(add)};

  return out;
}

template<std::integral T>
T
capped_add(T a, T b)
{
  T out;
  if (integer_add_overflow(a, b, &out)) {
    if (a > 0) {
      assert(b > 0);
      return std::numeric_limits<T>::max();
    }
    // else a < 0
    assert(a < 0);
    assert(b < 0);
    return std::numeric_limits<T>::min();
  }
  return out;
}

} // namespace

std::optional<StorageObjectType>
ItemView::get_object_type() const
{
  return std::visit(
      [](auto const &v) -> std::optional<StorageObjectType> {
        return v.get_object_type();
      },
      delta_args_);
}

std::optional<StorageDelta>
ItemView::get_delta() const
{
  return std::visit(
      [](auto const &v) -> std::optional<StorageDelta> {
        return v.get_delta();
      },
      delta_args_);
}

StorageDelta
BytestringItem::get_delta() const
{
  return make_bytestring_set(set_bytestring_arg_);
}

StorageDelta
NNIntType::get_delta() const
{
  return make_nnint128_set_add(nnint128_set_arg_, nnint128_add_sum_);
}

SyscallStatus<void> 
BytestringItem::validate() const
{
  if (bytestring_.size() > kBytestringMaxLen) {
    return SyscallError{SyscallErrorCode::kInvalidSyscallInput};
  }
}


} // namespace hogd
