/* Runtime support for the C code that `x23c --c` generates.
 *
 * Everything here is static inline and needs only the freestanding headers
 * <stdint.h>, <stdbool.h> and <stddef.h>: no libc, no allocator.  Define
 * X23_MEMCPY before including this header to use your own memcpy; the
 * default is a byte loop.  (GCC and Clang may still lower that loop, or any
 * struct copy, to a call to memcpy, which they expect every environment to
 * provide.)
 *
 * The header compiles as C11 or later and as C++20 or later.
 *
 * Decoding is zero-copy where the wire format allows it:
 *
 *   - opaque<N> and string<N> decode to an x23_bytes / x23_string view that
 *     points into the input buffer.
 *   - T<N> decodes to a T_vec, and T* to a T_opt.  Both are views of the
 *     encoded bytes: decoding validates every element once and records where
 *     the encoding lies; T_vec_get / T_vec_next / T_opt_get decode elements on
 *     demand.  Indexing is O(1) when T's encoding has a fixed size and a walk
 *     from the start otherwise.
 *   - Everything else (integers, enums, structs, unions, fixed-length arrays)
 *     decodes into native fields, since XDR is big-endian and padded.
 *
 * A decoded value therefore borrows the input buffer, which must outlive it.
 *
 * To encode a value you build yourself, point a T_vec at a C array (elems,
 * len, with xdr == NULL) and a T_opt at a single value (ptr, with xdr ==
 * NULL).  A view that came from decoding (xdr != NULL) re-encodes by copying
 * its already-validated bytes.
 *
 * Decoding accepts the encodings that the x23 C++ deserializer accepts, with
 * two differences: a bool union discriminant must be 0 or 1 (X23_ERR_BOOL),
 * as in the Rust bindings, whereas C++ takes any nonzero value as TRUE; and
 * nesting is bounded by X23_NESTING_LIMIT rather than C++'s recursion limit.
 * As in both, an enum holds any int32, declared or not; the generated
 * T_valid() tells whether it is one of T's constants.
 */

#ifndef X23_XDR_C_H
#define X23_XDR_C_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
#define X23_C_LINKAGE_BEGIN extern "C" {
#define X23_C_LINKAGE_END }
#else
#define X23_C_LINKAGE_BEGIN
#define X23_C_LINKAGE_END
#endif

X23_C_LINKAGE_BEGIN

/* Status codes.  Every encode and decode function returns one. */
typedef int32_t x23_status;
#define X23_OK 0
#define X23_ERR_SHORT 1    /* input ended before the value did */
#define X23_ERR_PADDING 2  /* nonzero padding byte */
#define X23_ERR_BOOL 3     /* bool (or optional flag) other than 0 or 1 */
#define X23_ERR_UNION 4    /* union discriminant selects no arm */
#define X23_ERR_LENGTH 5   /* length exceeds the declared bound */
#define X23_ERR_DEPTH 6    /* views nested beyond X23_NESTING_LIMIT */
#define X23_ERR_TRAILING 7 /* bytes left over after a complete value */
#define X23_ERR_ALIGN 8    /* input length not a multiple of 4 */
#define X23_ERR_OVERFLOW 9 /* output buffer too small */
#define X23_ERR_RANGE 10   /* vec index out of range, or absent optional */

/* Maximum nesting of views (T_vec, T_opt, T_box) while decoding.  Types can
 * recurse only through views, and decoding one recurses on the C stack, so
 * this bounds the stack that hostile input can use.  It counts every view
 * level, recursive or not, so it is not the x23 C++ deserializer's limit. */
#ifndef X23_NESTING_LIMIT
#define X23_NESTING_LIMIT 32
#endif

#define X23_TRY(expr)                                                          \
  do {                                                                         \
    x23_status x23_try_status_ = (expr);                                       \
    if (x23_try_status_ != X23_OK)                                             \
      return x23_try_status_;                                                  \
  } while (0)

/* Unbounded vecs and strings. */
#define X23_UNBOUNDED UINT32_MAX

typedef struct x23_reader {
  const uint8_t *p;
  const uint8_t *end;
  uint32_t depth; /* remaining nesting budget */
} x23_reader;

/* A writer with buf == NULL writes nothing and only counts bytes into pos,
 * which is how to size an output buffer. */
typedef struct x23_writer {
  uint8_t *buf;
  uint32_t pos;
  uint32_t cap;
} x23_writer;

/* opaque<N>: a view of the bytes, not including padding. */
typedef struct x23_bytes {
  const uint8_t *data;
  uint32_t len;
} x23_bytes;

/* string<N>: a view of the characters.  Not NUL-terminated. */
typedef struct x23_string {
  const char *data;
  uint32_t len;
} x23_string;

static inline x23_reader
x23_reader_init(const uint8_t *buf, uint32_t len)
{
  x23_reader r;
  r.p = buf;
  r.end = buf + len;
  r.depth = X23_NESTING_LIMIT;
  return r;
}

static inline x23_writer
x23_writer_init(uint8_t *buf, uint32_t cap)
{
  x23_writer w;
  w.buf = buf;
  w.pos = 0;
  w.cap = cap;
  return w;
}

static inline void
x23_copy_bytes(uint8_t *dst, const uint8_t *src, uint32_t n)
{
#ifdef X23_MEMCPY
  X23_MEMCPY(dst, src, n);
#else
  for (uint32_t i = 0; i < n; ++i)
    dst[i] = src[i];
#endif
}

static inline uint32_t
x23_padded_len(uint32_t n)
{
  return (n + 3u) & ~3u;
}

static inline uint32_t
x23_remaining(const x23_reader *r)
{
  return (uint32_t)(r->end - r->p);
}

static inline x23_status
x23_nest(x23_reader *r)
{
  if (r->depth == 0)
    return X23_ERR_DEPTH;
  --r->depth;
  return X23_OK;
}

static inline void
x23_unnest(x23_reader *r)
{
  ++r->depth;
}

/* Reading */

static inline x23_status
x23_get_u32(x23_reader *r, uint32_t *out)
{
  if (x23_remaining(r) < 4)
    return X23_ERR_SHORT;
  const uint8_t *p = r->p;
  *out = (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 |
         (uint32_t)p[3];
  r->p += 4;
  return X23_OK;
}

static inline x23_status
x23_get_u64(x23_reader *r, uint64_t *out)
{
  uint32_t hi, lo;
  X23_TRY(x23_get_u32(r, &hi));
  X23_TRY(x23_get_u32(r, &lo));
  *out = (uint64_t)hi << 32 | lo;
  return X23_OK;
}

/* Takes n bytes of opaque data plus its padding, returning a pointer to the
 * data in the input buffer. */
static inline x23_status
x23_get_opaque(x23_reader *r, uint32_t n, const uint8_t **out)
{
  uint32_t padded = x23_padded_len(n);
  if (padded < n || x23_remaining(r) < padded)
    return X23_ERR_SHORT;
  for (uint32_t i = n; i < padded; ++i)
    if (r->p[i] != 0)
      return X23_ERR_PADDING;
  *out = r->p;
  r->p += padded;
  return X23_OK;
}

/* Writing */

static inline x23_status
x23_reserve(x23_writer *w, uint32_t n, uint8_t **out)
{
  if (n > UINT32_MAX - w->pos)
    return X23_ERR_OVERFLOW;
  if (w->buf && n > w->cap - w->pos)
    return X23_ERR_OVERFLOW;
  *out = w->buf ? w->buf + w->pos : NULL;
  w->pos += n;
  return X23_OK;
}

static inline x23_status
x23_put_u32(x23_writer *w, uint32_t v)
{
  uint8_t *p;
  X23_TRY(x23_reserve(w, 4, &p));
  if (p) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
  }
  return X23_OK;
}

static inline x23_status
x23_put_u64(x23_writer *w, uint64_t v)
{
  X23_TRY(x23_put_u32(w, (uint32_t)(v >> 32)));
  return x23_put_u32(w, (uint32_t)v);
}

/* Bytes that are already a complete XDR encoding (no padding added). */
static inline x23_status
x23_put_raw(x23_writer *w, const uint8_t *src, uint32_t n)
{
  uint8_t *p;
  X23_TRY(x23_reserve(w, n, &p));
  if (p && n)
    x23_copy_bytes(p, src, n);
  return X23_OK;
}

/* n bytes of opaque data followed by zero padding. */
static inline x23_status
x23_put_opaque(x23_writer *w, const uint8_t *src, uint32_t n)
{
  uint32_t padded = x23_padded_len(n);
  uint8_t *p;
  if (padded < n)
    return X23_ERR_OVERFLOW;
  X23_TRY(x23_reserve(w, padded, &p));
  if (p) {
    if (n)
      x23_copy_bytes(p, src, n);
    for (uint32_t i = n; i < padded; ++i)
      p[i] = 0;
  }
  return X23_OK;
}

/* Variable-length opaque and string */

static inline x23_status
x23_bytes_decode(x23_reader *r, x23_bytes *out, uint32_t bound)
{
  uint32_t n;
  X23_TRY(x23_get_u32(r, &n));
  if (n > bound)
    return X23_ERR_LENGTH;
  X23_TRY(x23_get_opaque(r, n, &out->data));
  out->len = n;
  return X23_OK;
}

static inline x23_status
x23_bytes_encode(x23_writer *w, const x23_bytes *v, uint32_t bound)
{
  if (v->len > bound)
    return X23_ERR_LENGTH;
  X23_TRY(x23_put_u32(w, v->len));
  return x23_put_opaque(w, v->data, v->len);
}

static inline x23_status
x23_string_decode(x23_reader *r, x23_string *out, uint32_t bound)
{
  x23_bytes b;
  X23_TRY(x23_bytes_decode(r, &b, bound));
  out->data = (const char *)b.data;
  out->len = b.len;
  return X23_OK;
}

static inline x23_status
x23_string_encode(x23_writer *w, const x23_string *v, uint32_t bound)
{
  x23_bytes b;
  b.data = (const uint8_t *)v->data;
  b.len = v->len;
  return x23_bytes_encode(w, &b, bound);
}

/* Primitive types.  Each type P has
 *   P_XDR_SIZE                      encoded size, or 0 if it varies
 *   x23_status P_decode(x23_reader *, T *)
 *   x23_status P_encode(x23_writer *, const T *)
 * and the P_vec and P_opt views below.  Generated types follow the same
 * scheme under their own names. */

#define x23_i32_XDR_SIZE 4
#define x23_u32_XDR_SIZE 4
#define x23_i64_XDR_SIZE 8
#define x23_u64_XDR_SIZE 8
#define x23_bool_XDR_SIZE 4
#define x23_f32_XDR_SIZE 4
#define x23_f64_XDR_SIZE 8

static inline x23_status
x23_u32_decode(x23_reader *r, uint32_t *out)
{
  return x23_get_u32(r, out);
}
static inline x23_status
x23_u32_encode(x23_writer *w, const uint32_t *v)
{
  return x23_put_u32(w, *v);
}
static inline x23_status
x23_i32_decode(x23_reader *r, int32_t *out)
{
  uint32_t u;
  X23_TRY(x23_get_u32(r, &u));
  *out = (int32_t)u;
  return X23_OK;
}
static inline x23_status
x23_i32_encode(x23_writer *w, const int32_t *v)
{
  return x23_put_u32(w, (uint32_t)*v);
}
static inline x23_status
x23_u64_decode(x23_reader *r, uint64_t *out)
{
  return x23_get_u64(r, out);
}
static inline x23_status
x23_u64_encode(x23_writer *w, const uint64_t *v)
{
  return x23_put_u64(w, *v);
}
static inline x23_status
x23_i64_decode(x23_reader *r, int64_t *out)
{
  uint64_t u;
  X23_TRY(x23_get_u64(r, &u));
  *out = (int64_t)u;
  return X23_OK;
}
static inline x23_status
x23_i64_encode(x23_writer *w, const int64_t *v)
{
  return x23_put_u64(w, (uint64_t)*v);
}
static inline x23_status
x23_bool_decode(x23_reader *r, bool *out)
{
  uint32_t u;
  X23_TRY(x23_get_u32(r, &u));
  if (u > 1)
    return X23_ERR_BOOL;
  *out = u != 0;
  return X23_OK;
}
static inline x23_status
x23_bool_encode(x23_writer *w, const bool *v)
{
  return x23_put_u32(w, *v ? 1u : 0u);
}
static inline x23_status
x23_f32_decode(x23_reader *r, float *out)
{
  union {
    uint32_t u;
    float f;
  } c;
  X23_TRY(x23_get_u32(r, &c.u));
  *out = c.f;
  return X23_OK;
}
static inline x23_status
x23_f32_encode(x23_writer *w, const float *v)
{
  union {
    uint32_t u;
    float f;
  } c;
  c.f = *v;
  return x23_put_u32(w, c.u);
}
static inline x23_status
x23_f64_decode(x23_reader *r, double *out)
{
  union {
    uint64_t u;
    double f;
  } c;
  X23_TRY(x23_get_u64(r, &c.u));
  *out = c.f;
  return X23_OK;
}
static inline x23_status
x23_f64_encode(x23_writer *w, const double *v)
{
  union {
    uint64_t u;
    double f;
  } c;
  c.f = *v;
  return x23_put_u64(w, c.u);
}

/* Views of variable-length arrays (T<N>) and optionals (T*).
 *
 * X23_VIEW_TYPES(P, T) declares the struct tags P_vec, P_vec_iter and P_opt.
 * It needs only a declaration of T, so it can precede T's definition (which
 * recursive types require).  X23_VIEW_FUNCS(P, T) defines their functions
 * and must follow P_decode, P_encode and P_XDR_SIZE.  X23_VIEW_PROTOS(P, T)
 * declares the functions that encoders and decoders use.
 *
 * P_vec: if xdr != NULL, the view holds len elements encoded in the xdr_len
 * bytes at xdr (not including the length prefix); otherwise it holds the len
 * values at elems.
 *
 * P_opt: if xdr != NULL, the value is present and encoded in the xdr_len bytes
 * at xdr; otherwise it is present iff ptr != NULL.
 *
 * P_box: a value that is always present, encoded exactly as a plain T, but
 * held like a P_opt.  The generator uses it for fields annotated
 * #[cpp(box)]: types that contain themselves by value.
 */

#define X23_VIEW_TYPES(P, T)                                                   \
  typedef struct P##_vec {                                                     \
    const uint8_t *xdr;                                                        \
    uint32_t xdr_len;                                                          \
    uint32_t len;                                                              \
    const T *elems;                                                            \
  } P##_vec;                                                                   \
  typedef struct P##_vec_iter {                                                \
    x23_reader r;                                                              \
    const T *elems;                                                            \
    uint32_t i;                                                                \
    uint32_t len;                                                              \
  } P##_vec_iter;                                                              \
  typedef struct P##_opt {                                                     \
    const uint8_t *xdr;                                                        \
    uint32_t xdr_len;                                                          \
    const T *ptr;                                                              \
  } P##_opt;                                                                   \
  typedef struct P##_box {                                                     \
    const uint8_t *xdr;                                                        \
    uint32_t xdr_len;                                                          \
    const T *ptr;                                                              \
  } P##_box;

/* Prototypes of the view functions that generated encoders and decoders
 * call, so that they can precede X23_VIEW_FUNCS. */
#define X23_VIEW_PROTOS(P, T)                                                  \
  static inline x23_status P##_vec_decode(x23_reader *r, P##_vec *out,         \
                                          uint32_t bound);                     \
  static inline x23_status P##_vec_encode(x23_writer *w, const P##_vec *v,     \
                                          uint32_t bound);                     \
  static inline x23_status P##_opt_decode(x23_reader *r, P##_opt *out);        \
  static inline x23_status P##_opt_encode(x23_writer *w, const P##_opt *o);    \
  static inline x23_status P##_box_decode(x23_reader *r, P##_box *out);        \
  static inline x23_status P##_box_encode(x23_writer *w, const P##_box *b);

#define X23_VIEW_FUNCS(P, T)                                                   \
  static inline x23_status P##_vec_decode(x23_reader *r, P##_vec *out,         \
                                          uint32_t bound)                      \
  {                                                                            \
    uint32_t n;                                                                \
    X23_TRY(x23_get_u32(r, &n));                                               \
    if (n > bound)                                                             \
      return X23_ERR_LENGTH;                                                   \
    if (P##_XDR_SIZE &&                                                        \
        n > x23_remaining(r) / (P##_XDR_SIZE ? P##_XDR_SIZE : 1))              \
      return X23_ERR_SHORT;                                                    \
    const uint8_t *start = r->p;                                               \
    if (n) {                                                                   \
      X23_TRY(x23_nest(r));                                                    \
      for (uint32_t i = 0; i < n; ++i) {                                       \
        T tmp;                                                                 \
        X23_TRY(P##_decode(r, &tmp));                                          \
      }                                                                        \
      x23_unnest(r);                                                           \
    }                                                                          \
    out->xdr = start;                                                          \
    out->xdr_len = (uint32_t)(r->p - start);                                   \
    out->len = n;                                                              \
    out->elems = NULL;                                                         \
    return X23_OK;                                                             \
  }                                                                            \
  static inline x23_status P##_vec_encode(x23_writer *w, const P##_vec *v,     \
                                          uint32_t bound)                      \
  {                                                                            \
    if (v->len > bound)                                                        \
      return X23_ERR_LENGTH;                                                   \
    X23_TRY(x23_put_u32(w, v->len));                                           \
    if (v->xdr)                                                                \
      return x23_put_raw(w, v->xdr, v->xdr_len);                               \
    for (uint32_t i = 0; i < v->len; ++i)                                      \
      X23_TRY(P##_encode(w, &v->elems[i]));                                    \
    return X23_OK;                                                             \
  }                                                                            \
  static inline x23_status P##_vec_get(const P##_vec *v, uint32_t i, T *out)   \
  {                                                                            \
    if (i >= v->len)                                                           \
      return X23_ERR_RANGE;                                                    \
    if (!v->xdr) {                                                             \
      *out = v->elems[i];                                                      \
      return X23_OK;                                                           \
    }                                                                          \
    x23_reader r = x23_reader_init(v->xdr, v->xdr_len);                        \
    if (P##_XDR_SIZE)                                                          \
      r.p += (size_t)i * P##_XDR_SIZE;                                         \
    else                                                                       \
      for (uint32_t j = 0; j < i; ++j) {                                       \
        T skip;                                                                \
        X23_TRY(P##_decode(&r, &skip));                                        \
      }                                                                        \
    return P##_decode(&r, out);                                                \
  }                                                                            \
  static inline void P##_vec_iter_init(P##_vec_iter *it, const P##_vec *v)     \
  {                                                                            \
    it->r.p = it->r.end = v->xdr;                                              \
    it->r.depth = X23_NESTING_LIMIT;                                           \
    if (v->xdr)                                                                \
      it->r.end = v->xdr + v->xdr_len;                                         \
    it->elems = v->xdr ? NULL : v->elems;                                      \
    it->i = 0;                                                                 \
    it->len = v->len;                                                          \
  }                                                                            \
  /* Stores the next element in *out and returns true, or returns false at     \
   * the end. */                                                               \
  static inline bool P##_vec_next(P##_vec_iter *it, T *out)                    \
  {                                                                            \
    if (it->i >= it->len)                                                      \
      return false;                                                            \
    if (it->elems)                                                             \
      *out = it->elems[it->i];                                                 \
    else if (P##_decode(&it->r, out) != X23_OK) {                              \
      it->i = it->len;                                                         \
      return false;                                                            \
    }                                                                          \
    ++it->i;                                                                   \
    return true;                                                               \
  }                                                                            \
  static inline bool P##_opt_present(const P##_opt *o)                         \
  {                                                                            \
    return o->xdr || o->ptr;                                                   \
  }                                                                            \
  /* Returns X23_ERR_RANGE if the optional is absent. */                       \
  static inline x23_status P##_opt_get(const P##_opt *o, T *out)               \
  {                                                                            \
    if (o->xdr) {                                                              \
      x23_reader r = x23_reader_init(o->xdr, o->xdr_len);                      \
      return P##_decode(&r, out);                                              \
    }                                                                          \
    if (!o->ptr)                                                               \
      return X23_ERR_RANGE;                                                    \
    *out = *o->ptr;                                                            \
    return X23_OK;                                                             \
  }                                                                            \
  static inline x23_status P##_opt_decode(x23_reader *r, P##_opt *out)         \
  {                                                                            \
    bool present;                                                              \
    X23_TRY(x23_bool_decode(r, &present));                                     \
    out->xdr = NULL;                                                           \
    out->xdr_len = 0;                                                          \
    out->ptr = NULL;                                                           \
    if (present) {                                                             \
      const uint8_t *start = r->p;                                             \
      T tmp;                                                                   \
      X23_TRY(x23_nest(r));                                                    \
      X23_TRY(P##_decode(r, &tmp));                                            \
      x23_unnest(r);                                                           \
      out->xdr = start;                                                        \
      out->xdr_len = (uint32_t)(r->p - start);                                 \
    }                                                                          \
    return X23_OK;                                                             \
  }                                                                            \
  static inline x23_status P##_opt_encode(x23_writer *w, const P##_opt *o)     \
  {                                                                            \
    if (o->xdr) {                                                              \
      X23_TRY(x23_put_u32(w, 1));                                              \
      return x23_put_raw(w, o->xdr, o->xdr_len);                               \
    }                                                                          \
    X23_TRY(x23_put_u32(w, o->ptr ? 1u : 0u));                                 \
    return o->ptr ? P##_encode(w, o->ptr) : X23_OK;                            \
  }                                                                            \
  static inline x23_status P##_box_get(const P##_box *b, T *out)               \
  {                                                                            \
    if (b->xdr) {                                                              \
      x23_reader r = x23_reader_init(b->xdr, b->xdr_len);                      \
      return P##_decode(&r, out);                                              \
    }                                                                          \
    if (!b->ptr)                                                               \
      return X23_ERR_RANGE;                                                    \
    *out = *b->ptr;                                                            \
    return X23_OK;                                                             \
  }                                                                            \
  static inline x23_status P##_box_decode(x23_reader *r, P##_box *out)         \
  {                                                                            \
    const uint8_t *start = r->p;                                               \
    T tmp;                                                                     \
    X23_TRY(x23_nest(r));                                                      \
    X23_TRY(P##_decode(r, &tmp));                                              \
    x23_unnest(r);                                                             \
    out->xdr = start;                                                          \
    out->xdr_len = (uint32_t)(r->p - start);                                   \
    out->ptr = NULL;                                                           \
    return X23_OK;                                                             \
  }                                                                            \
  /* Returns X23_ERR_RANGE if the box holds nothing. */                        \
  static inline x23_status P##_box_encode(x23_writer *w, const P##_box *b)     \
  {                                                                            \
    if (b->xdr)                                                                \
      return x23_put_raw(w, b->xdr, b->xdr_len);                               \
    return b->ptr ? P##_encode(w, b->ptr) : X23_ERR_RANGE;                     \
  }

/* X23_BUF_FUNCS(P, T) defines, for a type whose P_decode and P_encode exist,
 *
 *   x23_status P_from_xdr(const uint8_t *buf, uint32_t len, T *out)
 *     decodes a whole buffer, which must hold exactly one value
 *   x23_status P_to_xdr(const T *v, uint8_t *buf, uint32_t cap, uint32_t *len)
 *     encodes v into buf and stores the encoded length in *len; with
 *     buf == NULL it writes nothing and only computes the length
 */
#define X23_BUF_FUNCS(P, T)                                                    \
  static inline x23_status P##_from_xdr(const uint8_t *buf, uint32_t len,      \
                                        T *out)                                \
  {                                                                            \
    if (len % 4)                                                               \
      return X23_ERR_ALIGN;                                                    \
    if (!buf)                                                                  \
      return X23_ERR_SHORT;                                                    \
    x23_reader r = x23_reader_init(buf, len);                                  \
    X23_TRY(P##_decode(&r, out));                                              \
    return r.p == r.end ? X23_OK : X23_ERR_TRAILING;                           \
  }                                                                            \
  static inline x23_status P##_to_xdr(const T *v, uint8_t *buf, uint32_t cap,  \
                                      uint32_t *len)                           \
  {                                                                            \
    x23_writer w = x23_writer_init(buf, cap);                                  \
    X23_TRY(P##_encode(&w, v));                                                \
    *len = w.pos;                                                              \
    return X23_OK;                                                             \
  }

X23_VIEW_TYPES(x23_i32, int32_t)
X23_VIEW_TYPES(x23_u32, uint32_t)
X23_VIEW_TYPES(x23_i64, int64_t)
X23_VIEW_TYPES(x23_u64, uint64_t)
X23_VIEW_TYPES(x23_bool, bool)
X23_VIEW_TYPES(x23_f32, float)
X23_VIEW_TYPES(x23_f64, double)
X23_VIEW_FUNCS(x23_i32, int32_t)
X23_VIEW_FUNCS(x23_u32, uint32_t)
X23_VIEW_FUNCS(x23_i64, int64_t)
X23_VIEW_FUNCS(x23_u64, uint64_t)
X23_VIEW_FUNCS(x23_bool, bool)
X23_VIEW_FUNCS(x23_f32, float)
X23_VIEW_FUNCS(x23_f64, double)

X23_C_LINKAGE_END

#endif /* !X23_XDR_C_H */
