// crypto.h - cryptographic utilities

#pragma once

extern "C" {
#include "blake3.h"
}

#include <openssl/rand.h> // NOLINT(misc-include-cleaner)
#include <openssl/sha.h>  // NOLINT(misc-include-cleaner)

#include <array>
#include <cassert>
#include <cstddef>
#include <span>
#include <version>

#include "x23/serialize.h"
#include "x23/vec.h"
#include "x23/x23.h"
#include <crc32c/crc32c.h>
#include <utility>

#include "hogd/base/compiler.h"
#include "hogd/base/types.h"
#include "hogd/rust/crypto/bindings.h"
#include "types.hh"
#include <sodium.h>

namespace hogd {

// read_random fills buf with high-quality random bytes (e.g., /dev/urandom)
// suitable for creating secret keys.
inline void
read_random(x23::is_bytespan auto buf)
{
  if (RAND_bytes(reinterpret_cast<uint8_t *>(buf.data()), buf.size()) != 1) {
    throw std::runtime_error("Failed to read random bytes");
  }
}

// sha256 returns the SHA256 hash of buf.
inline SHA256Hash
sha256(x23::is_bytespan auto buf)
{
  // We need to directly use lower-level functions in OpenSSL even though they
  // are deprecated. The public API (i.e., the [EVP_*] functions) do reference
  // counting and acquire locks, which limits parallelism and also harms
  // single-thread performance.
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
  SHA256Hash out;
  SHA256_CTX ctx;
  SHA256_Init(&ctx);
  SHA256_Update(&ctx, buf.data(), buf.size());
  SHA256_Final(out.data(), &ctx);
  return out;
}

template<x23::is_xdr_type T> requires (!x23::is_bytespan<T>)
inline SHA256Hash
sha256(const T &value)
{
  return sha256(x23::serialize(value));
}

using Blake3Hash = std::array<unsigned char, BLAKE3_OUT_LEN>;

inline Blake3Hash
blake3(x23::is_bytespan auto buf)
{
  Blake3Hash out;
  blake3_hasher hasher;
  blake3_hasher_init(&hasher);
  blake3_hasher_update(&hasher, buf.data(), buf.size());
  blake3_hasher_finalize(&hasher, out.data(), sizeof(out));
  return out;
}

template<x23::is_xdr_type T> requires (!x23::is_bytespan<T>)
inline Blake3Hash
blake3(const T &value)
{
  return blake3(x23::serialize(value));
}

// SipHash24Key is the secret key used by SipHash24.
using SipHash24Key = std::array<uint64_t, 2>;

// SipHash24 is a non-cryptographic hash function that is designed to prevent
// denial of service attacks by making it more difficult for an adversary to
// find collisions that cause load imbalances. SipHash24 requires a secret key,
// which will be randomly generated if not specified.
class alignas(kCacheLineSize) SipHash24 {
public:
  // get_default returns a default, process-wide SipHash24 that was initialized
  // with a random key. In most cases, you should use this unless you need a
  // specific key.
  static SipHash24 &get_default()
  {
    static SipHash24 hasher;
    return hasher;
  }

  SipHash24() { read_random(std::as_writable_bytes(std::span(key_))); }
  SipHash24(SipHash24Key key) : key_(key) {}
  ~SipHash24() = default;

  // Returns the hash of the input bytes.
  uint64_t operator()(x23::is_bytespan auto buf);

  SipHash24Key get_key() const { return key_; }

private:
  // the secret key.
  SipHash24Key key_;
};

// confirm our types are the same as libsodium.
// static_assert(sizeof(SipHash24Key) == crypto_shorthash_siphash24_KEYBYTES);
// static_assert(sizeof(uint64_t) == crypto_shorthash_siphash24_BYTES);

uint64_t
SipHash24::operator()(x23::is_bytespan auto buf)
{
  return siphash24_hash(key_[0], key_[1],
                        reinterpret_cast<const uint8_t *>(buf.data()),
                        buf.size());
}

// ed25519_keygen generates a new ed25519 keypair.
inline std::pair<Ed25519PublicKey, Ed25519SecretKey>
ed25519_keygen()
{
  Ed25519PublicKey pk;
  Ed25519SecretKey sk;
  static_assert(sizeof(Ed25519PublicKey) == ED25519_PUBLIC_KEY_SIZE);
  static_assert(sizeof(Ed25519SecretKey) == ED25519_SECRET_KEYPAIR_SIZE);
  std::array<uint8_t, 32> random_bytes;
  read_random(random_bytes);
  ed25519_dalek_keygen(random_bytes.data(), pk.data(), sk.data());
  //[[maybe_unused]] int ret = crypto_sign_ed25519_keypair(pk.data(),
  // sk.data());
  // this libsodium call cannot fail (V1.20).
  // assert(ret == 0);
  return {pk, sk};
}

inline bool
ed25519_verify(std::span<const std::byte, sizeof(Ed25519PublicKey)> public_key,
               std::span<const std::byte, sizeof(Ed25519Signature)> signature,
               x23::is_bytespan auto msg)
{
  return ed25519_dalek_verify(
             reinterpret_cast<const uint8_t (*)[32]>(public_key.data()),
             reinterpret_cast<const uint8_t (*)[64]>(signature.data()),
             reinterpret_cast<const uint8_t *>(msg.data()),
             msg.size()) == Ed25519Result::Success;
}

// ed25519_verify_rust verifies sig for a msg and publickey using Rust
// implementation.
inline bool
ed25519_verify(const Ed25519PublicKey &publickey, const Ed25519Signature &sig,
               x23::is_bytespan auto msg)
{
  return ed25519_dalek_verify(
             reinterpret_cast<const uint8_t (*)[32]>(publickey.data()),
             reinterpret_cast<const uint8_t (*)[64]>(sig.data()),
             reinterpret_cast<const uint8_t *>(msg.data()),
             msg.size()) == Ed25519Result::Success;
}

inline Ed25519Signature
ed25519_sign(const Ed25519SecretKey &sk, x23::is_bytespan auto msg)
{
  Ed25519Signature sig;
  [[maybe_unused]] uint8_t ret =
      ed25519_dalek_sign(reinterpret_cast<const uint8_t (*)[64]>(sk.data()),
                         reinterpret_cast<const uint8_t *>(msg.data()),
                         msg.size(),
                         reinterpret_cast<uint8_t (*)[64]>(sig.data()));
  assert(ret == 0);
  return sig;
}

// crc32c calculates the CRC32C of buf.
inline uint32_t
crc32c(x23::is_bytespan auto buf)
{
  return crc32c::Crc32c(reinterpret_cast<const uint8_t *>(buf.data()),
                        buf.size());
}

// crc32c_append extends a previous CRC32C to include buf.
inline uint32_t
crc32c_append(uint32_t crc, x23::is_bytespan auto buf)
{
  return crc32c::Extend(crc, reinterpret_cast<const uint8_t *>(buf.data()),
                        buf.size());
}

} // namespace hogd
