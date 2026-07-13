// Fuzz test for XDR and CXS deserialization.
//
// Tests that arbitrary byte sequences never cause undefined behavior,
// memory corruption, or unexpected exceptions when deserialized as
// various XDR types. Parse errors are expected for invalid input; any other
// exception or crash indicates a real bug.

#include <cstddef>
#include <cstdint>
#include <span>

#include <x23/serialize.h>
#include <x23/serialize_cxs.h>
#include <x23/vec.h>

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

// Strict XDR deserialization: rejects trailing data and non-4-aligned input.
// Exercises different error paths than deserialize_from.
template<typename T>
void
try_xdr_strict(const uint8_t *data, size_t size)
{
  T val{};
  try {
    x23::Buf buf(data, data + size);
    x23::BufViewEnd view(buf);
    x23::deserialize(&val, view);
  } catch (const x23::XdrException &) {
    // Expected for invalid input
  }
}

template<typename T>
void
try_cxs(const uint8_t *data, size_t size)
{
  T val{};
  std::span<const uint8_t> sp(data, size);
  (void)x23::deserialize_cxs(&val, sp);
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

  // optional types
  try_xdr<int_opt>(data, size);
  try_xdr<int_opt_vec>(data, size);
  try_xdr<int_opt_opt>(data, size);

  try_cxs<int_opt>(data, size);
  try_cxs<int_opt_vec>(data, size);
  try_cxs<int_opt_opt>(data, size);

  // --- Previously untested types from xdrtest.x ---

  // Float primitive
  try_xdr<float>(data, size);
  try_cxs<float>(data, size);

  // Float in struct
  try_xdr<float_struct>(data, size);
  try_cxs<float_struct>(data, size);

  // Union without default void case (no fallback arm)
  try_xdr<sunion_no_void>(data, size);
  try_cxs<sunion_no_void>(data, size);

  // Union with int discriminant and struct arms
  try_xdr<u_4_12>(data, size);
  try_cxs<u_4_12>(data, size);

  // Vector of primitives
  try_xdr<intvec>(data, size);
  try_cxs<intvec>(data, size);

  // Struct with only int64_t fields
  try_xdr<NonnegativeInt64SetAddArgs>(data, size);
  try_cxs<NonnegativeInt64SetAddArgs>(data, size);

  // Empty struct
  try_xdr<VoidCheck>(data, size);
  try_cxs<VoidCheck>(data, size);

  // Struct with void fields interspersed with data
  try_xdr<VoidCheckStrange>(data, size);
  try_cxs<VoidCheckStrange>(data, size);

  // Simple single-field structs (int64_t, uint64_t)
  try_xdr<MultilineComment>(data, size);
  try_xdr<StellarValue>(data, size);
  try_xdr<MultilineCommentPost>(data, size);

  try_cxs<MultilineComment>(data, size);
  try_cxs<StellarValue>(data, size);
  try_cxs<MultilineCommentPost>(data, size);

  // Struct with fixed opaque array + variable opaque vector
  try_xdr<TxEmittedEvent>(data, size);
  try_cxs<TxEmittedEvent>(data, size);

  // Union directly (not just via StorageDeltaVec)
  try_xdr<StorageDelta>(data, size);
  try_cxs<StorageDelta>(data, size);

  // --- New types for comprehensive coverage ---

  // Bounded typed vector (max-size validation on non-opaque)
  try_xdr<bounded_intvec>(data, size);
  try_cxs<bounded_intvec>(data, size);

  // Fixed-size array of structs
  try_xdr<array_of_structs>(data, size);
  try_cxs<array_of_structs>(data, size);

  // Fixed-size array of primitives
  try_xdr<array_of_prims>(data, size);
  try_cxs<array_of_prims>(data, size);

  // Vectors of unions
  try_xdr<coloru_vec>(data, size);
  try_xdr<storage_delta_vec2>(data, size);

  try_cxs<coloru_vec>(data, size);
  try_cxs<storage_delta_vec2>(data, size);

  // Struct containing a union field
  try_xdr<struct_with_union>(data, size);
  try_cxs<struct_with_union>(data, size);

  // Struct with multiple variable-length fields
  try_xdr<multi_vec_struct>(data, size);
  try_cxs<multi_vec_struct>(data, size);

  // Optional struct
  try_xdr<opt_struct>(data, size);
  try_cxs<opt_struct>(data, size);

  // Optional union
  try_xdr<opt_union>(data, size);
  try_cxs<opt_union>(data, size);

  // Optional string
  try_xdr<opt_string_ptr>(data, size);
  try_cxs<opt_string_ptr>(data, size);

  // Vector of vectors (nested dynamic)
  try_xdr<vec_of_vecs>(data, size);
  try_cxs<vec_of_vecs>(data, size);

  // Vector of optional structs
  try_xdr<opt_fix4_vec>(data, size);
  try_cxs<opt_fix4_vec>(data, size);

  // Union with many arms
  try_xdr<many_arm_union>(data, size);
  try_cxs<many_arm_union>(data, size);

  // Union where all arms are void
  try_xdr<all_void_union>(data, size);
  try_cxs<all_void_union>(data, size);

  // Deeply nested structs (3 levels)
  try_xdr<inner_struct>(data, size);
  try_xdr<middle_struct>(data, size);
  try_xdr<outer_struct>(data, size);

  try_cxs<inner_struct>(data, size);
  try_cxs<middle_struct>(data, size);
  try_cxs<outer_struct>(data, size);

  // Union with complex struct arms containing vectors
  try_xdr<complex_arm_union>(data, size);
  try_cxs<complex_arm_union>(data, size);

  // Enum vector (standalone enum deserialization)
  try_xdr<gappy_enum_vec>(data, size);
  try_cxs<gappy_enum_vec>(data, size);

  // Non-aligned opaque arrays (padding validation)
  try_xdr<opaque_1>(data, size);
  try_xdr<opaque_5>(data, size);
  try_xdr<opaque_7>(data, size);
  try_xdr<opaque_33>(data, size);

  try_cxs<opaque_1>(data, size);
  try_cxs<opaque_5>(data, size);
  try_cxs<opaque_7>(data, size);
  try_cxs<opaque_33>(data, size);

  // Bounded string (max-size rejection for string type)
  try_xdr<bounded_string>(data, size);
  try_cxs<bounded_string>(data, size);

  // Union with default arm carrying data (not void)
  try_xdr<default_data_union>(data, size);
  try_cxs<default_data_union>(data, size);

  // Union with bool discriminant, both arms with data
  try_xdr<bool_both_arms>(data, size);
  try_cxs<bool_both_arms>(data, size);

  // Struct mixing fixed-opaque arrays of various alignments
  try_xdr<mixed_opaque_struct>(data, size);
  try_cxs<mixed_opaque_struct>(data, size);

  // Single-element bounded vector (max_size == 1)
  try_xdr<single_vec>(data, size);
  try_cxs<single_vec>(data, size);

  // Very small bounded opaque
  try_xdr<tiny_opaque>(data, size);
  try_cxs<tiny_opaque>(data, size);

  // --- Strict XDR deserialization (rejects trailing data, non-4-aligned) ---
  // Test a representative subset of types through the strict path.
  try_xdr_strict<int32_t>(data, size);
  try_xdr_strict<bool>(data, size);
  try_xdr_strict<fix_12>(data, size);
  try_xdr_strict<sunion>(data, size);
  try_xdr_strict<uunion>(data, size);
  try_xdr_strict<x23::Str>(data, size);
  try_xdr_strict<Data>(data, size);
  try_xdr_strict<ContractAddress>(data, size);
  try_xdr_strict<GenericArraySize>(data, size);
  try_xdr_strict<StorageDelta>(data, size);
  try_xdr_strict<TxEventLog>(data, size);
  try_xdr_strict<test_recursive>(data, size);
  try_xdr_strict<default_data_union>(data, size);
  try_xdr_strict<many_arm_union>(data, size);
  try_xdr_strict<outer_struct>(data, size);

  return 0;
}
