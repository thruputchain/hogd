# C output of x23c

`x23c --c` compiles a `.x` file to a C header holding its types and,
for each type, functions that encode and decode it in traditional XDR
and in compact XDR (CXS).  It is meant for code such as
contracts that cannot use C++; RPC `program` definitions are skipped.

    x23c --c -o myprot.h myprot.x

The generated header includes `<x23/xdr_c.h>`, the runtime, which is
header-only C11 and needs only `<stdint.h>`, `<stdbool.h>` and
`<stddef.h>`, plus `malloc` and `free` unless you supply an allocator
(see [Memory](#memory)).  It uses `__builtin_memcpy`, so it builds with
GCC or Clang, freestanding included.  Every function is `static`, so
there is nothing to link, and what you do not call is not compiled in
(see [Code size](#code-size)).

## XDR types

| XDR type | C type |
|:---------|:-------|
| `bool` | `bool` |
| `int` | `int32_t` |
| `unsigned int` | `uint32_t` |
| `hyper` | `int64_t` |
| `unsigned hyper` | `uint64_t` |
| `float` | `float` |
| `double` | `double` |
| `quadruple` | `quadruple`, which you define (see below) |
| `string<N>` | `x23_string` |
| `opaque[N]` | `uint8_t[N]` |
| `opaque<N>` | `x23_bytes` |
| `T[N]` | `T[N]` |
| `T<N>` | `T_vec` |
| `T*` | `T *`, `NULL` when absent |
| `enum E` | `typedef int32_t E` plus constants `E_NAME` |
| `struct` | `struct` |
| `union` | `struct` holding the discriminant and an anonymous `union` |

* `x23_string` is `{char *data; uint32_t len;}`, and `x23_bytes` is
  `{uint8_t *data; uint32_t len;}`.  A decoded string is followed by a
  NUL byte not counted in `len`, so `data` can be used as a C string
  unless the string itself holds a NUL.

* `T_vec` is `{T *elems; uint32_t len;}`.  The primitives have their
  own, such as `x23_i32_vec` for `int<>`.

* Enums are `int32_t`, and their constants are prefixed with the enum's
  name (`enum Color { RED = 0 }` gives `Color_RED`), since C would
  otherwise reject two enums that share a name.  Decoding accepts any
  value; `E_valid(v)` says whether `v` is one of the declared ones.

* A union `union Shape switch (Color kind)` becomes a `struct Shape`
  with a `Color kind` field and an anonymous `union` holding one member
  per arm that has a value.  Set `kind` and then the matching member.
  `bool` unions only accept 0 and 1 as discriminants.

* A `typedef opaque Hash[32]` becomes `struct {uint8_t bytes[32];}`, so
  values of the type can be assigned and passed by value.  Opaque arrays
  written directly as fields stay plain `uint8_t[N]`.

* Anonymous structs and unions nested in a definition are given names
  from their path, such as `Outer_field`.

* A field named after a C keyword (`default`, `auto`, ...) gets a
  trailing `_`.

* Constants become `#define`s.

### Recursive types

A recursive type has to recurse through a pointer.  An optional `T*`
already is one.  A field that holds its own type by value must be
marked `// #[cpp(box)]`, as for the C++ output:

```c
struct ArrayT {
  TypeT element; // #[cpp(box)]
  unsigned int len;
};
```

The C field is then `TypeT *element`.  Unlike an optional, it is never
`NULL` once decoded, has no presence flag on the wire, and encoding a
`NULL` one fails with `X23_ERR_NULL`.

### `quadruple`

No C type is a 128-bit float everywhere, so x23c leaves `quadruple` for
you to define before including the generated header, along with
`quadruple_decode_xdr`, `quadruple_encode_xdr`, their `_cxs`
counterparts and `quadruple_free` (see
[Lower-level functions](#lower-level-functions)), and
`X23_VEC_TYPE(quadruple, quadruple)`, `X23_PTR_PROTOS(quadruple,
quadruple)` and `X23_PTR_FUNCS(quadruple, quadruple)` if it appears in
vectors or optionals.

### Literal lines

C has no use for the C++ in `%` lines, so they are dropped, except
`%#include "other.hh"`, which becomes `#include "other.h"`.  That
includes forward declarations: C does not need them, since the header
declares every struct and union before defining any.

## Functions

For each type `T` the header defines:

```c
x23_status T_from_xdr(const uint8_t *buf, uint32_t len, T *out);
x23_status T_to_xdr(const T *v, uint8_t *buf, uint32_t cap, uint32_t *len);
x23_status T_from_cxs(const uint8_t *buf, uint32_t len, T *out);
x23_status T_to_cxs(const T *v, uint8_t *buf, uint32_t cap, uint32_t *len);
void T_free(T *v);
```

* `T_from_*` decodes all of `buf` into `*out`: bytes left over are an
  error (`X23_ERR_TRAILING`), as is a traditional XDR input whose length
  is not a multiple of 4.

* `T_to_*` encodes `*v` into `buf`, of `cap` bytes, and sets `*len` to
  the length.  With `buf == NULL` it writes nothing and only computes
  `*len`, which is how to size the buffer.

* `T_free` frees what decoding allocated (see [Memory](#memory)).

Every encode and decode function returns `X23_OK` (0) or an error code
from `xdr_c.h`, such as `X23_ERR_SHORT` (the input ended early),
`X23_ERR_LENGTH` (a length over its bound), `X23_ERR_OVERFLOW` (the
output buffer is too small) or `X23_ERR_NOMEM`.

```c
#include "myprot.h"

x23_status
round_trip(const uint8_t *in, uint32_t in_len, uint8_t *out, uint32_t cap)
{
  Thing t;
  uint32_t len;
  X23_TRY(Thing_from_cxs(in, in_len, &t));
  x23_status st = Thing_to_cxs(&t, out, cap, &len);
  Thing_free(&t);
  return st;
}
```

`X23_TRY(expr)` returns from the calling function if `expr` is not
`X23_OK`.

### Lower-level functions

The buffer functions above are built on functions that work on a
position in a larger buffer:

```c
x23_status T_decode_xdr(x23_reader *r, T *out);
x23_status T_encode_xdr(x23_writer *w, const T *v);
x23_status T_decode_cxs(x23_reader *r, T *out);
x23_status T_encode_cxs(x23_writer *w, const T *v);
```

`x23_reader_init(buf, len)` and `x23_writer_init(buf, cap)` make
readers and writers, which are the same for both formats: the format is
in the function's name.  The decode functions do not check for trailing
bytes.  Along with them, each again with an `_xdr` and a `_cxs` form,
come `T_vec_decode` and `T_vec_encode` (taking the bound, or
`X23_UNBOUNDED`), `T_ptr_decode` and `T_ptr_encode` for optionals, and
`T_box_decode` and `T_box_encode` for boxed fields.  `T_vec_free` and
`T_ptr_free` (which also frees boxed fields) serve both formats.

## Code size

Each codec exists once per format, so the format is fixed at compile
time, and since the functions are `static`, the compiler drops the ones
a program does not call.  A contract that only uses CXS carries no
traditional XDR code.

Functions are declared with `X23_FN`, which is `static inline` except
on rv32, where it is plain `static`: without the hint, the compiler
inlines only where that makes the code smaller, as the Rust `xdrs`
crate arranges for contract builds.  Define `X23_FN` before including
the header to choose otherwise.

## Memory

Decoding copies everything into memory the value owns: strings, opaque
data, vector elements and pointed-to values.  `T_free` releases it and
leaves the value safe to free again.  If decoding fails, whatever it
had allocated is freed before it returns.  Encoding allocates nothing.

Memory comes from `X23_MALLOC(size)` and goes back with
`X23_FREE(ptr)`, which default to `malloc` and `free`.  To use your own
allocator, define them before including the header.  If you define
`X23_MALLOC` and not `X23_FREE`, freeing does nothing, which suits a
bump allocator that is reset as a whole:

```c
#define X23_MALLOC(size) bump_alloc(size)
#include "myprot.h"
```

`X23_MEMCPY(dst, src, n)` can be overridden the same way.

Lengths are only checked against their declared bounds, so an
unbounded length in hostile input can ask for a large allocation (and
fail with `X23_ERR_NOMEM`), and deeply nested recursive values use
stack in proportion to their depth.  The code is meant for input such
as contract calldata, where a call that runs out of memory or stack
just faults.
