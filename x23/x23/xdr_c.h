/* Runtime for the C that `x23c --c` generates.  Decoded values own their
 * memory, allocated with X23_MALLOC (malloc unless defined; X23_FREE then
 * defaults to a no-op), and T_free() releases it. */

#ifndef X23_XDR_C_H
#define X23_XDR_C_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifndef X23_MALLOC
#include <stdlib.h>
#define X23_MALLOC(size) malloc(size)
#ifndef X23_FREE
#define X23_FREE(ptr) free(ptr)
#endif
#endif
#ifndef X23_FREE
#define X23_FREE(ptr) ((void)(ptr))
#endif
#ifndef X23_MEMCPY
#define X23_MEMCPY(dst, src, n) __builtin_memcpy(dst, src, n)
#endif

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
#define X23_ERR_TRAILING 6 /* bytes left over after a complete value */
#define X23_ERR_ALIGN 7    /* input length not a multiple of 4 */
#define X23_ERR_OVERFLOW 8 /* output buffer too small */
#define X23_ERR_NULL 9     /* encoding a NULL #[cpp(box)] field */
#define X23_ERR_NOMEM 10   /* X23_MALLOC failed */

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
} x23_reader;

/* A writer with buf == NULL writes nothing and only counts bytes into pos,
 * which is how to size an output buffer. */
typedef struct x23_writer {
  uint8_t *buf;
  uint32_t pos;
  uint32_t cap;
} x23_writer;

/* opaque<N> */
typedef struct x23_bytes {
  uint8_t *data;
  uint32_t len;
} x23_bytes;

/* string<N> */
typedef struct x23_string {
  char *data;
  uint32_t len;
} x23_string;

static inline x23_reader
x23_reader_init(const uint8_t *buf, uint32_t len)
{
  x23_reader r;
  r.p = buf;
  r.end = buf + len;
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

static inline void
x23_zero(void *p, size_t n)
{
  __builtin_memset(p, 0, n);
}

/* Allocates n zeroed objects of the given size. */
static inline x23_status
x23_alloc(void **out, uint32_t n, size_t size)
{
  if (size && n > SIZE_MAX / size)
    return X23_ERR_NOMEM;
  size_t total = (size_t)n * size;
  void *p = X23_MALLOC(total ? total : 1);
  if (!p)
    return X23_ERR_NOMEM;
  x23_zero(p, total);
  *out = p;
  return X23_OK;
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

/* Copies n bytes of opaque data into dst and skips its padding. */
static inline x23_status
x23_get_opaque(x23_reader *r, uint32_t n, uint8_t *dst)
{
  uint32_t padded = x23_padded_len(n);
  if (padded < n || x23_remaining(r) < padded)
    return X23_ERR_SHORT;
  for (uint32_t i = n; i < padded; ++i)
    if (r->p[i] != 0)
      return X23_ERR_PADDING;
  if (n)
    X23_MEMCPY(dst, r->p, n);
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
      X23_MEMCPY(p, src, n);
    for (uint32_t i = n; i < padded; ++i)
      p[i] = 0;
  }
  return X23_OK;
}

/* Variable-length opaque and string */

/* Reads the length and checks the bytes are there before allocating, so a
 * huge length in short input cannot allocate. */
static inline x23_status
x23_get_opaque_len(x23_reader *r, uint32_t bound, uint32_t *n)
{
  X23_TRY(x23_get_u32(r, n));
  if (*n > bound)
    return X23_ERR_LENGTH;
  uint32_t padded = x23_padded_len(*n);
  if (padded < *n || x23_remaining(r) < padded)
    return X23_ERR_SHORT;
  return X23_OK;
}

static inline x23_status
x23_bytes_decode(x23_reader *r, x23_bytes *out, uint32_t bound)
{
  uint32_t n;
  void *p;
  out->data = NULL;
  out->len = 0;
  X23_TRY(x23_get_opaque_len(r, bound, &n));
  X23_TRY(x23_alloc(&p, n, 1));
  out->data = (uint8_t *)p;
  out->len = n;
  return x23_get_opaque(r, n, out->data);
}

static inline x23_status
x23_bytes_encode(x23_writer *w, const x23_bytes *v, uint32_t bound)
{
  if (v->len > bound)
    return X23_ERR_LENGTH;
  X23_TRY(x23_put_u32(w, v->len));
  return x23_put_opaque(w, v->data, v->len);
}

static inline void
x23_bytes_free(x23_bytes *v)
{
  X23_FREE(v->data);
  v->data = NULL;
  v->len = 0;
}

static inline x23_status
x23_string_decode(x23_reader *r, x23_string *out, uint32_t bound)
{
  uint32_t n;
  void *p;
  out->data = NULL;
  out->len = 0;
  X23_TRY(x23_get_opaque_len(r, bound, &n));
  if (n == UINT32_MAX)
    return X23_ERR_NOMEM;
  X23_TRY(x23_alloc(&p, n + 1, 1)); /* zeroed, so NUL-terminated */
  out->data = (char *)p;
  out->len = n;
  return x23_get_opaque(r, n, (uint8_t *)out->data);
}

static inline x23_status
x23_string_encode(x23_writer *w, const x23_string *v, uint32_t bound)
{
  if (v->len > bound)
    return X23_ERR_LENGTH;
  X23_TRY(x23_put_u32(w, v->len));
  return x23_put_opaque(w, (const uint8_t *)v->data, v->len);
}

static inline void
x23_string_free(x23_string *v)
{
  X23_FREE(v->data);
  v->data = NULL;
  v->len = 0;
}

/* Primitives, with the same P_XDR_SIZE (0 if it varies), P_decode, P_encode
 * and P_free that generated types have. */

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
/* Also decodes bool union discriminants, which C++ (unlike Rust) does not
 * restrict to 0 or 1. */
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
#define X23_NOTHING_TO_FREE(P, T)                                              \
  static inline void P##_free(T *v)                                            \
  {                                                                            \
    (void)v;                                                                   \
  }
X23_NOTHING_TO_FREE(x23_i32, int32_t)
X23_NOTHING_TO_FREE(x23_u32, uint32_t)
X23_NOTHING_TO_FREE(x23_i64, int64_t)
X23_NOTHING_TO_FREE(x23_u64, uint64_t)
X23_NOTHING_TO_FREE(x23_bool, bool)
X23_NOTHING_TO_FREE(x23_f32, float)
X23_NOTHING_TO_FREE(x23_f64, double)

/* T<N> (P_vec), T* (P_ptr, NULL when absent) and #[cpp(box)] fields (P_box).
 * X23_VEC_TYPE needs only a declaration of T, as recursive types require. */

#define X23_VEC_TYPE(P, T)                                                     \
  typedef struct P##_vec {                                                     \
    T *elems;                                                                  \
    uint32_t len;                                                              \
  } P##_vec;

#define X23_PTR_PROTOS(P, T)                                                   \
  static inline x23_status P##_vec_decode(x23_reader *r, P##_vec *out,         \
                                          uint32_t bound);                     \
  static inline x23_status P##_vec_encode(x23_writer *w, const P##_vec *v,     \
                                          uint32_t bound);                     \
  static inline void P##_vec_free(P##_vec *v);                                 \
  static inline x23_status P##_ptr_decode(x23_reader *r, T **out);             \
  static inline x23_status P##_ptr_encode(x23_writer *w, const T *v);          \
  static inline void P##_ptr_free(T **v);                                      \
  static inline x23_status P##_box_decode(x23_reader *r, T **out);             \
  static inline x23_status P##_box_encode(x23_writer *w, const T *v);

/* A variable-size encoding is at least 4 bytes, so a count is checked against
 * the input before allocating for it. */
#define X23_PTR_FUNCS(P, T)                                                    \
  static inline x23_status P##_vec_decode(x23_reader *r, P##_vec *out,         \
                                          uint32_t bound)                      \
  {                                                                            \
    uint32_t n;                                                                \
    void *p;                                                                   \
    out->elems = NULL;                                                         \
    out->len = 0;                                                              \
    X23_TRY(x23_get_u32(r, &n));                                               \
    if (n > bound)                                                             \
      return X23_ERR_LENGTH;                                                   \
    if (n > x23_remaining(r) / (P##_XDR_SIZE ? P##_XDR_SIZE : 4))              \
      return X23_ERR_SHORT;                                                    \
    X23_TRY(x23_alloc(&p, n, sizeof(T)));                                      \
    out->elems = (T *)p;                                                       \
    out->len = n;                                                              \
    for (uint32_t i = 0; i < n; ++i)                                           \
      X23_TRY(P##_decode(r, &out->elems[i]));                                  \
    return X23_OK;                                                             \
  }                                                                            \
  static inline x23_status P##_vec_encode(x23_writer *w, const P##_vec *v,     \
                                          uint32_t bound)                      \
  {                                                                            \
    if (v->len > bound)                                                        \
      return X23_ERR_LENGTH;                                                   \
    X23_TRY(x23_put_u32(w, v->len));                                           \
    for (uint32_t i = 0; i < v->len; ++i)                                      \
      X23_TRY(P##_encode(w, &v->elems[i]));                                    \
    return X23_OK;                                                             \
  }                                                                            \
  static inline void P##_vec_free(P##_vec *v)                                  \
  {                                                                            \
    if (v->elems)                                                              \
      for (uint32_t i = 0; i < v->len; ++i)                                    \
        P##_free(&v->elems[i]);                                                \
    X23_FREE(v->elems);                                                        \
    v->elems = NULL;                                                           \
    v->len = 0;                                                                \
  }                                                                            \
  static inline x23_status P##_box_decode(x23_reader *r, T **out)              \
  {                                                                            \
    void *p;                                                                   \
    *out = NULL;                                                               \
    X23_TRY(x23_alloc(&p, 1, sizeof(T)));                                      \
    *out = (T *)p;                                                             \
    return P##_decode(r, *out);                                                \
  }                                                                            \
  static inline x23_status P##_box_encode(x23_writer *w, const T *v)           \
  {                                                                            \
    return v ? P##_encode(w, v) : X23_ERR_NULL;                                \
  }                                                                            \
  static inline x23_status P##_ptr_decode(x23_reader *r, T **out)              \
  {                                                                            \
    bool present;                                                              \
    *out = NULL;                                                               \
    X23_TRY(x23_bool_decode(r, &present));                                     \
    return present ? P##_box_decode(r, out) : X23_OK;                          \
  }                                                                            \
  static inline x23_status P##_ptr_encode(x23_writer *w, const T *v)           \
  {                                                                            \
    X23_TRY(x23_put_u32(w, v ? 1u : 0u));                                      \
    return v ? P##_encode(w, v) : X23_OK;                                      \
  }                                                                            \
  static inline void P##_ptr_free(T **v)                                       \
  {                                                                            \
    if (*v) {                                                                  \
      P##_free(*v);                                                            \
      X23_FREE(*v);                                                            \
      *v = NULL;                                                               \
    }                                                                          \
  }

/* P_from_xdr decodes a buffer holding exactly one value (owning nothing on
 * error); P_to_xdr with buf == NULL only computes the length. */
#define X23_BUF_FUNCS(P, T)                                                    \
  static inline x23_status P##_from_xdr(const uint8_t *buf, uint32_t len,      \
                                        T *out)                                \
  {                                                                            \
    x23_zero(out, sizeof(T));                                                  \
    if (len % 4)                                                               \
      return X23_ERR_ALIGN;                                                    \
    if (!buf)                                                                  \
      return X23_ERR_SHORT;                                                    \
    x23_reader r = x23_reader_init(buf, len);                                  \
    x23_status st = P##_decode(&r, out);                                       \
    if (st == X23_OK && r.p != r.end)                                          \
      st = X23_ERR_TRAILING;                                                   \
    if (st != X23_OK)                                                          \
      P##_free(out);                                                           \
    return st;                                                                 \
  }                                                                            \
  static inline x23_status P##_to_xdr(const T *v, uint8_t *buf, uint32_t cap,  \
                                      uint32_t *len)                           \
  {                                                                            \
    x23_writer w = x23_writer_init(buf, cap);                                  \
    X23_TRY(P##_encode(&w, v));                                                \
    *len = w.pos;                                                              \
    return X23_OK;                                                             \
  }

X23_VEC_TYPE(x23_i32, int32_t)
X23_VEC_TYPE(x23_u32, uint32_t)
X23_VEC_TYPE(x23_i64, int64_t)
X23_VEC_TYPE(x23_u64, uint64_t)
X23_VEC_TYPE(x23_bool, bool)
X23_VEC_TYPE(x23_f32, float)
X23_VEC_TYPE(x23_f64, double)
X23_PTR_FUNCS(x23_i32, int32_t)
X23_PTR_FUNCS(x23_u32, uint32_t)
X23_PTR_FUNCS(x23_i64, int64_t)
X23_PTR_FUNCS(x23_u64, uint64_t)
X23_PTR_FUNCS(x23_bool, bool)
X23_PTR_FUNCS(x23_f32, float)
X23_PTR_FUNCS(x23_f64, double)

X23_C_LINKAGE_END

#endif /* !X23_XDR_C_H */
