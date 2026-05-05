# x23::Box<T> — Non-Nullable Heap Wrapper for Recursive Types

**Date:** 2026-05-04

## Problem

Recursive XDR types currently require `Ptr<T>` (`option<T>`) to break the
static size dependency. `Ptr<T>` is nullable, meaning every recursive reference
carries a null state that the type system doesn't actually want. This is
analogous to Rust requiring `Option<Box<T>>` when you really just want `Box<T>`.

We want a non-nullable heap-allocated wrapper that:

- Breaks recursive sizing like `Ptr<T>` does
- Serializes transparently as `T` on the wire (no presence flag)
- Lives outside the XDR category system (this is a representation concern, not a
  wire concept)

## Design

### `x23::Box<T>` (in `x23.h`, alongside `Ptr<T>`)

A `unique_ptr<T>` wrapper that is never null.

**Construction and assignment:**

- Default constructor allocates and value-initializes `T` (required because the
  deserializer default-constructs structs then overwrites fields in place — same
  double-initialization pattern as every other field type)
- Copy constructor deep-copies via `new T(*other)` (same as `Ptr<T>`)
- Move constructor takes ownership (same as `unique_ptr`)
- Construction from rvalue `T` and `std::in_place_t` variants (same as `Ptr<T>`)
- No construction from `nullptr`

**Dereference:**

- `operator*`, `operator->`, `get()` — all forward to inner `unique_ptr`
- No `operator bool` — it's always true

**Comparison:**

- `operator==` and `operator<=>` dereference both sides and delegate
- No null case (unlike `Ptr<T>`)

### Wire Transparency

`Box<T>` is invisible on the wire. A field of type `Box<T>` serializes
identically to a field of type `T`.

- `serialize_equiv<T, Box<T>>` declared as `std::true_type`
- Free-function `xdr_serialize(serializer, Box<T>)` dereferences and delegates
- Free-function `xdr_size(Box<T>)` dereferences and delegates

### Framework Integration (No New XdrCategory)

`Box<T>` does not introduce a new `XdrCategory`. Instead:

- `xdr_meta<Box<T>>` forwards `T`'s metadata (category, type_name, field_names,
  etc.)
- `Box<T>` implements the tuple protocol by forwarding through the pointer:
  - `std::tuple_size<Box<T>>` = `std::tuple_size<T>`
  - `std::tuple_element<I, Box<T>>` = `std::tuple_element<I, T>`
  - `get<I>(box)` returns `get<I>(*box)`
- `visit_fields` works on `Box<T>` because it uses the tuple protocol
- The serializer and deserializer are not modified — they see `Box<T>` as `T`
  through the forwarded metadata and tuple protocol

This means `xdr_size_bounds`, recursion detection (`xdr_contains`,
`is_recursive_xdr`), and deserialization all work without changes to the
serialization framework internals.

### x23c Codegen

Per-language annotations in end-of-line comments on scalar fields (no `*`
pointer syntax). The XDR parser doesn't enforce struct sizing, so a scalar
self-referential field is valid XDR — the annotations tell each backend to
heap-allocate:

```xdr
struct TreeNode {
  int32 value;
  TreeNode left;  // #[cpp(box)] #[rust(box_ptr)]
  TreeNode right; // #[cpp(box)] #[rust(box_ptr)]
};
```

**C++ (`gen_hh.cc`):**

When `#[cpp(box)]` is present on a `kScalar` field, emit `x23::Box<T>` instead
of bare `T`.

**Rust (`gen_rs.cc`):**

No changes needed. The existing `#[rust(box_ptr)]` annotation (and the
`recursive_types` set) already handle `kScalar` fields by emitting `Box<T>`.

**TypeScript (`gen_ts.cc`):**

No changes. TypeScript/JavaScript doesn't have the static sizing constraint, so
recursive types don't need special wrapping.

### Annotation Parsing

Follows the existing pattern from `gen_rs.cc`:

- Annotations live in the `comment` field of `Declaration`
- Parsed via `substring_removed()` utility (or equivalent)
- Each backend checks for its own annotation and strips it from the output
  comment

New constant:

```cpp
// gen_hh.cc
static constexpr string_view kCppBox = R"(#[cpp(box)])";
```

## Scope

This spec covers:

1. `x23::Box<T>` type definition in `x23/x23/x23.h`
2. Serialization support (`serialize_equiv`, `xdr_serialize`, `xdr_size`) in
   `x23/x23/serialize.h`
3. `xdr_meta` forwarding and tuple protocol in `x23/x23/x23.h`
4. `#[cpp(box)]` annotation support in `x23/x23c/gen_hh.cc`
5. Tests for serialization round-trip, non-nullability, and recursive type usage

This spec does not cover:

- Rust codegen changes (existing `#[rust(box_ptr)]` already handles `Box<T>`)
- TypeScript codegen changes
- Changes to the `Serializer` or `Deserializer` classes
- New `XdrCategory` values
- Changes to the XDR grammar / parser (annotations are comments, not syntax)
