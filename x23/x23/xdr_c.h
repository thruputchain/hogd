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
#define X23_ERR_SHORT 1         /* input ended before the value did */
#define X23_ERR_PADDING 2       /* nonzero padding byte */
#define X23_ERR_BOOL 3          /* bool (or optional flag) other than 0 or 1 */
#define X23_ERR_UNION 4         /* union discriminant selects no arm */
#define X23_ERR_LENGTH 5        /* length exceeds the declared bound */
#define X23_ERR_TRAILING 6      /* bytes left over after a complete value */
#define X23_ERR_ALIGN 7         /* input length not a multiple of 4 */
#define X23_ERR_OVERFLOW 8      /* output buffer too small */
#define X23_ERR_NULL 9          /* encoding a NULL #[cpp(box)] field */
#define X23_ERR_NOMEM 10        /* X23_MALLOC failed */
#define X23_ERR_NONCANONICAL 11 /* CXS integer not in its shortest form */

#define X23_TRY(expr)                                                          \
  do {                                                                         \
    x23_status x23_try_status_ = (expr);                                       \
    if (x23_try_status_ != X23_OK)                                             \
      return x23_try_status_;                                                  \
  } while (0)

/* Unbounded vecs and strings. */
#define X23_UNBOUNDED UINT32_MAX

/* cxs selects compact XDR (CXS) over traditional XDR. */
typedef struct x23_reader {
  const uint8_t *p;
  const uint8_t *end;
  bool cxs;
} x23_reader;

/* A writer with buf == NULL writes nothing and only counts bytes into pos,
 * which is how to size an output buffer. */
typedef struct x23_writer {
  uint8_t *buf;
  uint32_t pos;
  uint32_t cap;
  bool cxs;
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
x23_reader_init(const uint8_t *buf, uint32_t len, bool cxs)
{
  x23_reader r;
  r.p = buf;
  r.end = buf + len;
  r.cxs = cxs;
  return r;
}

static inline x23_writer
x23_writer_init(uint8_t *buf, uint32_t cap, bool cxs)
{
  x23_writer w;
  w.buf = buf;
  w.pos = 0;
  w.cap = cap;
  w.cxs = cxs;
  return w;
}

/* Opaque data is padded to 4 bytes in XDR and not at all in CXS. */
static inline uint32_t
x23_padded_len(bool cxs, uint32_t n)
{
  return cxs ? n : (n + 3u) & ~3u;
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
x23_take(x23_reader *r, uint32_t n, const uint8_t **out)
{
  if (x23_remaining(r) < n)
    return X23_ERR_SHORT;
  *out = r->p;
  r->p += n;
  return X23_OK;
}

static inline uint64_t
x23_load_be(const uint8_t *p, uint32_t n)
{
  uint64_t v = 0;
  for (uint32_t i = 0; i < n; ++i)
    v = v << 8 | p[i];
  return v;
}

static inline uint64_t
x23_load_le(const uint8_t *p, uint32_t n)
{
  uint64_t v = 0;
  for (uint32_t i = n; i-- > 0;)
    v = v << 8 | p[i];
  return v;
}

/* A fixed-width XDR integer, or a raw little-endian CXS float. */
static inline x23_status
x23_get_fixed(x23_reader *r, uint32_t n, uint64_t *out)
{
  const uint8_t *p;
  X23_TRY(x23_take(r, n, &p));
  *out = r->cxs ? x23_load_le(p, n) : x23_load_be(p, n);
  return X23_OK;
}

/* A CXS integer: a header byte holding the value itself (0..62 unsigned,
 * -31..31 signed), or 63 in its low 6 bits and 1, 2, 4 or 8 little-endian
 * bytes following (per its top 2 bits), whichever form is shortest. */
static inline x23_status
x23_cxs_payload(x23_reader *r, uint8_t h, uint64_t *v, uint32_t *n)
{
  const uint8_t *p;
  if ((h & 0x3f) != 63)
    return X23_ERR_NONCANONICAL;
  *n = 1u << (h >> 6);
  X23_TRY(x23_take(r, *n, &p));
  *v = x23_load_le(p, *n);
  return X23_OK;
}

static inline x23_status
x23_cxs_get_uint(x23_reader *r, uint64_t *out)
{
  const uint8_t *p;
  uint32_t n;
  X23_TRY(x23_take(r, 1, &p));
  if (p[0] <= 62) {
    *out = p[0];
    return X23_OK;
  }
  X23_TRY(x23_cxs_payload(r, p[0], out, &n));
  /* Each payload size must hold a value too big for the next smaller form. */
  uint64_t min = n == 1   ? 63
                 : n == 2 ? 0x100
                 : n == 4 ? 0x10000
                          : (uint64_t)0x100000000;
  return *out < min ? X23_ERR_NONCANONICAL : X23_OK;
}

static inline x23_status
x23_cxs_get_int(x23_reader *r, int64_t *out)
{
  const uint8_t *p;
  uint64_t u;
  uint32_t n;
  X23_TRY(x23_take(r, 1, &p));
  if (p[0] <= 62) {
    *out = p[0] <= 31 ? (int64_t)p[0] : -(int64_t)(~p[0] & 0x1f);
    return X23_OK;
  }
  X23_TRY(x23_cxs_payload(r, p[0], &u, &n));
  if (n < 8 && (u >> (8 * n - 1)) & 1)
    u |= ~(uint64_t)0 << (8 * n);
  *out = (int64_t)u;
  /* Each payload size must hold a value outside the next smaller form. */
  int64_t lo = n == 1   ? -31
               : n == 2 ? INT8_MIN
               : n == 4 ? INT16_MIN
                        : INT32_MIN;
  int64_t hi = n == 1 ? 31 : n == 2 ? INT8_MAX : n == 4 ? INT16_MAX : INT32_MAX;
  return *out >= lo && *out <= hi ? X23_ERR_NONCANONICAL : X23_OK;
}

static inline x23_status
x23_get_opaque(x23_reader *r, uint32_t n, uint8_t *dst)
{
  uint32_t padded = x23_padded_len(r->cxs, n);
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

static inline void
x23_store_be(uint8_t *p, uint64_t v, uint32_t n)
{
  for (uint32_t i = n; i-- > 0; v >>= 8)
    p[i] = (uint8_t)v;
}

static inline void
x23_store_le(uint8_t *p, uint64_t v, uint32_t n)
{
  for (uint32_t i = 0; i < n; ++i, v >>= 8)
    p[i] = (uint8_t)v;
}

static inline x23_status
x23_put_fixed(x23_writer *w, uint32_t n, uint64_t v)
{
  uint8_t *p;
  X23_TRY(x23_reserve(w, n, &p));
  if (p) {
    if (w->cxs)
      x23_store_le(p, v, n);
    else
      x23_store_be(p, v, n);
  }
  return X23_OK;
}

static inline x23_status
x23_cxs_put(x23_writer *w, uint32_t n, uint64_t v)
{
  static const uint8_t header[9] = {0, 63, 127, 0, 191, 0, 0, 0, 255};
  uint8_t *p;
  X23_TRY(x23_reserve(w, 1 + n, &p));
  if (p) {
    p[0] = header[n];
    x23_store_le(p + 1, v, n);
  }
  return X23_OK;
}

static inline x23_status
x23_cxs_put_byte(x23_writer *w, uint8_t b)
{
  uint8_t *p;
  X23_TRY(x23_reserve(w, 1, &p));
  if (p)
    *p = b;
  return X23_OK;
}

static inline x23_status
x23_cxs_put_uint(x23_writer *w, uint64_t v)
{
  if (v <= 62)
    return x23_cxs_put_byte(w, (uint8_t)v);
  return x23_cxs_put(w,
                     v <= 0xff         ? 1
                     : v <= 0xffff     ? 2
                     : v <= 0xffffffff ? 4
                                       : 8,
                     v);
}

static inline x23_status
x23_cxs_put_int(x23_writer *w, int64_t v)
{
  if (v >= -31 && v <= 31)
    return x23_cxs_put_byte(w, v >= 0
                                   ? (uint8_t)v
                                   : (uint8_t)((~(uint8_t)-v & 0x1f) | 0x20));
  return x23_cxs_put(w,
                     v >= INT8_MIN && v <= INT8_MAX     ? 1
                     : v >= INT16_MIN && v <= INT16_MAX ? 2
                     : v >= INT32_MIN && v <= INT32_MAX ? 4
                                                        : 8,
                     (uint64_t)v);
}

static inline x23_status
x23_put_opaque(x23_writer *w, const uint8_t *src, uint32_t n)
{
  uint32_t padded = x23_padded_len(w->cxs, n);
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

/* Primitives, with the same P_decode, P_encode and P_free that generated types
 * have. */

static inline x23_status
x23_u32_decode(x23_reader *r, uint32_t *out)
{
  uint64_t v;
  X23_TRY(r->cxs ? x23_cxs_get_uint(r, &v) : x23_get_fixed(r, 4, &v));
  if (v > UINT32_MAX)
    return X23_ERR_NONCANONICAL;
  *out = (uint32_t)v;
  return X23_OK;
}
static inline x23_status
x23_u32_encode(x23_writer *w, const uint32_t *v)
{
  return w->cxs ? x23_cxs_put_uint(w, *v) : x23_put_fixed(w, 4, *v);
}
static inline x23_status
x23_i32_decode(x23_reader *r, int32_t *out)
{
  if (r->cxs) {
    int64_t v;
    X23_TRY(x23_cxs_get_int(r, &v));
    if (v < INT32_MIN || v > INT32_MAX)
      return X23_ERR_NONCANONICAL;
    *out = (int32_t)v;
    return X23_OK;
  }
  uint64_t u;
  X23_TRY(x23_get_fixed(r, 4, &u));
  *out = (int32_t)(uint32_t)u;
  return X23_OK;
}
static inline x23_status
x23_i32_encode(x23_writer *w, const int32_t *v)
{
  return w->cxs ? x23_cxs_put_int(w, *v) : x23_put_fixed(w, 4, (uint32_t)*v);
}
static inline x23_status
x23_u64_decode(x23_reader *r, uint64_t *out)
{
  return r->cxs ? x23_cxs_get_uint(r, out) : x23_get_fixed(r, 8, out);
}
static inline x23_status
x23_u64_encode(x23_writer *w, const uint64_t *v)
{
  return w->cxs ? x23_cxs_put_uint(w, *v) : x23_put_fixed(w, 8, *v);
}
static inline x23_status
x23_i64_decode(x23_reader *r, int64_t *out)
{
  if (r->cxs)
    return x23_cxs_get_int(r, out);
  uint64_t u;
  X23_TRY(x23_get_fixed(r, 8, &u));
  *out = (int64_t)u;
  return X23_OK;
}
static inline x23_status
x23_i64_encode(x23_writer *w, const int64_t *v)
{
  return w->cxs ? x23_cxs_put_int(w, *v) : x23_put_fixed(w, 8, (uint64_t)*v);
}
/* Also decodes bool union discriminants, which XDR in C++ (unlike Rust) does
 * not restrict to 0 or 1. */
static inline x23_status
x23_bool_decode(x23_reader *r, bool *out)
{
  uint64_t v;
  X23_TRY(r->cxs ? x23_cxs_get_uint(r, &v) : x23_get_fixed(r, 4, &v));
  if (v > 1)
    return X23_ERR_BOOL;
  *out = v != 0;
  return X23_OK;
}
static inline x23_status
x23_bool_encode(x23_writer *w, const bool *v)
{
  return w->cxs ? x23_cxs_put_byte(w, *v ? 1 : 0)
                : x23_put_fixed(w, 4, *v ? 1 : 0);
}
/* Floats are raw IEEE 754 bits: big-endian in XDR, little-endian in CXS. */
static inline x23_status
x23_f32_decode(x23_reader *r, float *out)
{
  uint64_t v;
  uint32_t u;
  X23_TRY(x23_get_fixed(r, 4, &v));
  u = (uint32_t)v;
  X23_MEMCPY(out, &u, 4);
  return X23_OK;
}
static inline x23_status
x23_f32_encode(x23_writer *w, const float *v)
{
  uint32_t u;
  X23_MEMCPY(&u, v, 4);
  return x23_put_fixed(w, 4, u);
}
static inline x23_status
x23_f64_decode(x23_reader *r, double *out)
{
  uint64_t u;
  X23_TRY(x23_get_fixed(r, 8, &u));
  X23_MEMCPY(out, &u, 8);
  return X23_OK;
}
static inline x23_status
x23_f64_encode(x23_writer *w, const double *v)
{
  uint64_t u;
  X23_MEMCPY(&u, v, 8);
  return x23_put_fixed(w, 8, u);
}
/* Primitives own nothing, but generated code frees every field and element
 * through its type's P_free, so they need one too.  (void)v keeps compilers
 * from warning about the unused parameter. */
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

/* Variable-length opaque and string */

/* The bytes must be present, so checking before allocating costs nothing. */
static inline x23_status
x23_get_opaque_len(x23_reader *r, uint32_t bound, uint32_t *n)
{
  X23_TRY(x23_u32_decode(r, n));
  if (*n > bound)
    return X23_ERR_LENGTH;
  uint32_t padded = x23_padded_len(r->cxs, *n);
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
  X23_TRY(x23_u32_encode(w, &v->len));
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
  X23_TRY(x23_u32_encode(w, &v->len));
  return x23_put_opaque(w, (const uint8_t *)v->data, v->len);
}

static inline void
x23_string_free(x23_string *v)
{
  X23_FREE(v->data);
  v->data = NULL;
  v->len = 0;
}

/* T<N> (P_vec), T* (P_ptr, NULL when absent) and #[cpp(box)] fields (P_box,
 * never NULL, no presence flag).  X23_VEC_TYPE needs only a declaration of T,
 * as recursive types require. */

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

#define X23_PTR_FUNCS(P, T)                                                    \
  static inline x23_status P##_vec_decode(x23_reader *r, P##_vec *out,         \
                                          uint32_t bound)                      \
  {                                                                            \
    uint32_t n;                                                                \
    void *p;                                                                   \
    out->elems = NULL;                                                         \
    out->len = 0;                                                              \
    X23_TRY(x23_u32_decode(r, &n));                                            \
    if (n > bound)                                                             \
      return X23_ERR_LENGTH;                                                   \
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
    X23_TRY(x23_u32_encode(w, &v->len));                                       \
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
    bool present = v != NULL;                                                  \
    X23_TRY(x23_bool_encode(w, &present));                                     \
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

/* Whole-buffer helpers, in XDR (P_from_xdr, P_to_xdr) and CXS (P_from_cxs,
 * P_to_cxs).  Decoding owns nothing on error; encoding with buf == NULL only
 * computes the length. */
static inline x23_status
x23_check_buf(const uint8_t **buf, uint32_t len, bool cxs)
{
  static const uint8_t empty[1] = {0};
  if (!cxs && len % 4)
    return X23_ERR_ALIGN;
  if (!*buf) {
    if (len)
      return X23_ERR_SHORT;
    *buf = empty;
  }
  return X23_OK;
}

#define X23_BUF_FUNCS_IN(P, T, FMT, CXS)                                       \
  static inline x23_status P##_from_##FMT(const uint8_t *buf, uint32_t len,    \
                                          T *out)                              \
  {                                                                            \
    x23_zero(out, sizeof(T));                                                  \
    X23_TRY(x23_check_buf(&buf, len, CXS));                                    \
    x23_reader r = x23_reader_init(buf, len, CXS);                             \
    x23_status st = P##_decode(&r, out);                                       \
    if (st == X23_OK && r.p != r.end)                                          \
      st = X23_ERR_TRAILING;                                                   \
    if (st != X23_OK)                                                          \
      P##_free(out);                                                           \
    return st;                                                                 \
  }                                                                            \
  static inline x23_status P##_to_##FMT(const T *v, uint8_t *buf,              \
                                        uint32_t cap, uint32_t *len)           \
  {                                                                            \
    x23_writer w = x23_writer_init(buf, cap, CXS);                             \
    X23_TRY(P##_encode(&w, v));                                                \
    *len = w.pos;                                                              \
    return X23_OK;                                                             \
  }
#define X23_BUF_FUNCS(P, T)                                                    \
  X23_BUF_FUNCS_IN(P, T, xdr, false)                                           \
  X23_BUF_FUNCS_IN(P, T, cxs, true)

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
