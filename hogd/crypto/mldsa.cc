#include "hogd/crypto/mldsa.h"

#include <cstddef>

#include "mldsa_native.h"
#include "x23/vec.h"

namespace hogd {

static_assert(kMlDsa65PublicKeyBytes == MLDSA65_PUBLICKEYBYTES);
static_assert(kMlDsa65SignatureBytes == MLDSA65_BYTES);

bool
mldsa65_verify(const MlDsa65PublicKey &public_key, const MlDsa65Signature &sig,
               x23::BufView msg, x23::BufView ctx)
{
  // mldsa-native rejects an over-long context string too, but the limit is
  // FIPS 204's, so enforce it here rather than rely on that.
  if (ctx.size() > kMlDsaMaxContextBytes) {
    return false;
  }
  // mldsa-native's ML-DSA.Verify, named by the mldsa_native_verify build's
  // MLD_CONFIG_NAMESPACE_PREFIX (see CMakeLists.txt).
  return hogd_mldsa65_verify(sig.data(), msg.data(), msg.size(), ctx.data(),
                             ctx.size(), public_key.data()) == 0;
}

} // namespace hogd
