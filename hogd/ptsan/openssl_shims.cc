// openssl_shims.cc - PointerTagSanitizer shims for the system libcrypto
//
// See shim.h.

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/sha.h>

#include <cstddef>

#include "hogd/ptsan/shim.h"

using hogd::ptsan::raw;
using hogd::ptsan::UninstrumentedCall;

// hogd calls the SHA256_* functions, which OpenSSL 3 deprecates.
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"

HOGD_PTSAN_WRAPPED(SHA256_Init);
HOGD_PTSAN_WRAPPED(SHA256_Update);
HOGD_PTSAN_WRAPPED(SHA256_Final);
HOGD_PTSAN_WRAPPED(RAND_bytes);
HOGD_PTSAN_WRAPPED(OPENSSL_cleanse);
HOGD_PTSAN_WRAPPED(EVP_MD_CTX_new);
HOGD_PTSAN_WRAPPED(EVP_MD_CTX_free);
HOGD_PTSAN_WRAPPED(EVP_shake128);
HOGD_PTSAN_WRAPPED(EVP_DigestInit_ex);
HOGD_PTSAN_WRAPPED(EVP_DigestUpdate);
HOGD_PTSAN_WRAPPED(EVP_DigestFinalXOF);

extern "C" {

int
__wrap_SHA256_Init(SHA256_CTX *c)
{
  UninstrumentedCall call;
  return __real_SHA256_Init(raw(c));
}

int
__wrap_SHA256_Update(SHA256_CTX *c, const void *data, size_t len)
{
  UninstrumentedCall call;
  return __real_SHA256_Update(raw(c), raw(data), len);
}

int
__wrap_SHA256_Final(unsigned char *md, SHA256_CTX *c)
{
  UninstrumentedCall call;
  return __real_SHA256_Final(raw(md), raw(c));
}

int
__wrap_RAND_bytes(unsigned char *buf, int num)
{
  UninstrumentedCall call;
  return __real_RAND_bytes(raw(buf), num);
}

void
__wrap_OPENSSL_cleanse(void *ptr, size_t len)
{
  UninstrumentedCall call;
  __real_OPENSSL_cleanse(raw(ptr), len);
}

EVP_MD_CTX *
__wrap_EVP_MD_CTX_new()
{
  UninstrumentedCall call;
  return __real_EVP_MD_CTX_new();
}

void
__wrap_EVP_MD_CTX_free(EVP_MD_CTX *ctx)
{
  UninstrumentedCall call;
  __real_EVP_MD_CTX_free(raw(ctx));
}

const EVP_MD *
__wrap_EVP_shake128()
{
  UninstrumentedCall call;
  return __real_EVP_shake128();
}

int
__wrap_EVP_DigestInit_ex(EVP_MD_CTX *ctx, const EVP_MD *type, ENGINE *impl)
{
  UninstrumentedCall call;
  return __real_EVP_DigestInit_ex(raw(ctx), raw(type), raw(impl));
}

int
__wrap_EVP_DigestUpdate(EVP_MD_CTX *ctx, const void *d, size_t cnt)
{
  UninstrumentedCall call;
  return __real_EVP_DigestUpdate(raw(ctx), raw(d), cnt);
}

int
__wrap_EVP_DigestFinalXOF(EVP_MD_CTX *ctx, unsigned char *out, size_t outlen)
{
  UninstrumentedCall call;
  return __real_EVP_DigestFinalXOF(raw(ctx), raw(out), outlen);
}

} // extern "C"
