// Fuzz test for XDR and CXS deserialization.
//
// Tests that arbitrary byte sequences never cause undefined behavior,
// memory corruption, or unexpected exceptions when deserialized as
// various XDR types. XdrBadInput/CxsBadInput/XdrException are expected
// for invalid input; any other exception or crash indicates a real bug.

#include <cstddef>
#include <cstdint>
#include <span>

#include <x23/serialize.h>
#include <x23/serialize_cxs.h>

#include "tests/xdrtest.hh"

namespace {

template<typename T>
void
try_xdr(const uint8_t *data, size_t size)
{
  T val{};
  try {
    x23::Buf buf(data, data + size);
    x23::BufViewEnd view(buf);
    x23::deserialize_from(&val, view);
  } catch (const x23::XdrException &) {
    // Expected for invalid input
  }
}

template<typename T>
void
try_cxs(const uint8_t *data, size_t size)
{
  T val{};
  try {
    std::span<const uint8_t> sp(data, size);
    x23::deserialize_cxs(&val, sp);
  } catch (const x23::XdrException &) {
    // Expected for invalid input
  }
}

} // namespace

extern "C" int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
  // Primitives
  try_xdr<int32_t>(data, size);
  try_xdr<uint32_t>(data, size);
  try_xdr<int64_t>(data, size);
  try_xdr<uint64_t>(data, size);
  try_xdr<bool>(data, size);
  try_xdr<double>(data, size);

  try_cxs<int32_t>(data, size);
  try_cxs<uint32_t>(data, size);
  try_cxs<int64_t>(data, size);
  try_cxs<uint64_t>(data, size);
  try_cxs<bool>(data, size);
  try_cxs<double>(data, size);

  // Strings and opaque vectors
  try_xdr<x23::Str>(data, size);
  try_xdr<Bytestring>(data, size);
  try_xdr<Data>(data, size);

  try_cxs<x23::Str>(data, size);
  try_cxs<Bytestring>(data, size);
  try_cxs<Data>(data, size);

  // Fixed-size structs
  try_xdr<fix_4>(data, size);
  try_xdr<fix_12>(data, size);
  try_xdr<fix_16>(data, size);

  try_cxs<fix_4>(data, size);
  try_cxs<fix_12>(data, size);
  try_cxs<fix_16>(data, size);

  // Unions with enum discriminant
  try_xdr<sunion>(data, size);
  try_xdr<coloru>(data, size);

  try_cxs<sunion>(data, size);
  try_cxs<coloru>(data, size);

  // Unions with int discriminant
  try_xdr<uunion>(data, size);
  try_xdr<NonEnumTag>(data, size);

  try_cxs<uunion>(data, size);
  try_cxs<NonEnumTag>(data, size);

  // Union with void arms / pointer
  try_xdr<ExtensionPoint>(data, size);
  try_xdr<uptr>(data, size);

  try_cxs<ExtensionPoint>(data, size);
  try_cxs<uptr>(data, size);

  // Recursive types
  try_xdr<test_recursive>(data, size);
  try_xdr<contains_test_recursive>(data, size);

  try_cxs<test_recursive>(data, size);
  try_cxs<contains_test_recursive>(data, size);

  // Vectors of structs
  try_xdr<StorageDeltaVec>(data, size);
  try_xdr<v12>(data, size);

  try_cxs<StorageDeltaVec>(data, size);
  try_cxs<v12>(data, size);

  // Complex nested structs
  try_xdr<ParseFailure>(data, size);
  try_xdr<TxEventLog>(data, size);

  try_cxs<ParseFailure>(data, size);
  try_cxs<TxEventLog>(data, size);

  // Fixed-size opaque arrays
  try_xdr<ContractAddress>(data, size);
  try_xdr<GenericArraySize>(data, size);

  try_cxs<ContractAddress>(data, size);
  try_cxs<GenericArraySize>(data, size);

  return 0;
}
