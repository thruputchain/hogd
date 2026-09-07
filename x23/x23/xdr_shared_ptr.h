// -*-C++-*-

#pragma once

#include <memory>
#include <utility>

#include <x23/serialize.h>
#include <x23/x23.h>

namespace x23 {

// An owning pointer that serializes as an XDR optional -- a 4-byte presence
// tag followed by the object -- exactly as [x23::Ptr] does, but from a
// [std::shared_ptr].
//
// [x23::Ptr] is a [std::unique_ptr], so its value constructor takes the object
// *by value*: building a reply out of [x23::Ptr] deep-copies whatever it is
// pointing at.  Where the object is already owned by a [std::shared_ptr] --
// which is how a lookup into a shared cache hands one back -- that copy buys
// nothing.  Holding the [std::shared_ptr] in the reply keeps the object alive
// on its own, and x23 serializes a handler's return value while it is still in
// scope (the [make_record] calls in x23/rpc.h), so the reference outlives the
// bytes being written.
//
// The pointee must not be mutated while a reply holding it is in flight.
template<typename T> struct XdrSharedPtr : std::shared_ptr<const T> {
  using super = std::shared_ptr<const T>;
  using value_type = T;

  using super::super;

  XdrSharedPtr() = default;
  explicit XdrSharedPtr(super p) : super(std::move(p)) {}
};

template<typename T>
inline size_t
xdr_size(const XdrSharedPtr<T> &p)
{
  if (p) {
    return 4UZ + x23::xdr_size(*p);
  }
  return 4UZ;
}

template<typename T>
inline void
xdr_serialize(auto &serializer, const XdrSharedPtr<T> &p)
{
  if (p) {
    serializer.put32(1);
    serializer(*p);
  }
  else {
    serializer.put32(0);
  }
}

// Declares that an [XdrSharedPtr] may stand in wherever the wire type is a
// [Ptr].  x23 composes this through vectors itself, so declaring it for the
// element type is enough for a vector of them to answer an RPC whose declared
// return is a vector of optionals.
template<typename X>
struct serialize_equiv<Ptr<X>, XdrSharedPtr<X>> : std::true_type {};

} // namespace x23
