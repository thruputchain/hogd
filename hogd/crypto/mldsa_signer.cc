#include "hogd/crypto/mldsa_signer.h"

#include <openssl/crypto.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdlib>
#include <optional>
#include <span>
#include <tuple>

#include "hogd/base/types.h"
#include "hogd/crypto/crypto.h"
#include "hogd/crypto/mldsa.h"
#include "mldsa_native.h"
#include "x23/vec.h"

namespace hogd {

static_assert(kMlDsa65SecretKeyBytes == MLDSA65_SECRETKEYBYTES);
static_assert(std::tuple_size_v<MlDsaSeed> == MLDSA_SEEDBYTES);
static_assert(std::tuple_size_v<MlDsaRnd> == MLDSA_RNDBYTES);

namespace {

template<size_t N>
void
cleanse(std::array<uint8_t, N> &buf)
{
  OPENSSL_cleanse(buf.data(), buf.size());
}

} // namespace

// The hogd_mldsa65_signer_* functions are mldsa-native's, named by the
// mldsa_native_signer build's MLD_CONFIG_NAMESPACE_PREFIX (see
// CMakeLists.txt).

MlDsa65SigningKey
MlDsa65SigningKey::generate()
{
  MlDsaSeed seed;
  read_random(std::as_writable_bytes(std::span(seed)));
  MlDsa65SigningKey key = from_seed(seed);
  cleanse(seed);
  return key;
}

MlDsa65SigningKey
MlDsa65SigningKey::from_seed(const MlDsaSeed &seed)
{
  MlDsa65SigningKey key;
  // Key generation can only fail in configurations hogd does not build
  // (custom allocators, the FIPS 140-3 pairwise consistency test).
  if (hogd_mldsa65_signer_keypair_internal(key.public_key_.data(),
                                           key.secret_key_.data(),
                                           seed.data()) != 0) {
    std::abort();
  }
  return key;
}

MlDsa65SigningKey::MlDsa65SigningKey(MlDsa65SigningKey &&other) noexcept
  : public_key_(other.public_key_), secret_key_(other.secret_key_)
{
  cleanse(other.secret_key_);
}

MlDsa65SigningKey::~MlDsa65SigningKey()
{
  cleanse(secret_key_);
}

std::optional<MlDsa65Signature>
MlDsa65SigningKey::sign(x23::BufView msg, x23::BufView ctx) const
{
  MlDsaRnd rnd;
  read_random(std::as_writable_bytes(std::span(rnd)));
  auto sig = sign_with_rnd(msg, ctx, rnd);
  cleanse(rnd);
  return sig;
}

std::optional<MlDsa65Signature>
MlDsa65SigningKey::sign_with_rnd(x23::BufView msg, x23::BufView ctx,
                                 const MlDsaRnd &rnd) const
{
  if (ctx.size() > kMlDsaMaxContextBytes) {
    return std::nullopt;
  }
  // Pure ML-DSA signs the message behind the prefix
  // 0 || IntegerToBytes(|ctx|, 1) || ctx (FIPS 204, Algorithm 2, line 10).
  // mldsa65_verify builds the same prefix inside mldsa-native.
  std::array<uint8_t, 2 + kMlDsaMaxContextBytes> prefix{};
  prefix[0] = 0;
  prefix[1] = static_cast<uint8_t>(ctx.size());
  std::ranges::copy(ctx, prefix.begin() + 2);

  MlDsa65Signature sig;
  if (hogd_mldsa65_signer_signature_internal(sig.data(), msg.data(), msg.size(),
                                             prefix.data(), 2 + ctx.size(),
                                             rnd.data(), secret_key_.data(),
                                             0) != 0) {
    return std::nullopt;
  }
  return sig;
}

} // namespace hogd
