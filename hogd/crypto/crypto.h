// crypto.h - cryptographic utilities

#pragma once

#include <sodium.h>

#include <array>
#include <cassert>
#include <span>
#include <utility>

#include <crc32c/crc32c.h>

#include "hogd/base/compiler.h"
#include "hogd/base/types.h"
#include "types.hh"
#include "x23/serialize.h"
#include "x23/vec.h"
#include "x23/x23.h"

namespace hogd {

// read_random fills buf with high-quality random bytes (e.g., /dev/urandom)
// suitable for creating secret keys.
inline void
read_random(x23::is_bytespan auto buf)
{
  randombytes_buf(reinterpret_cast<uint8_t *>(buf.data()), buf.size());
}

// sha256 returns the SHA256 hash of buf.
inline SHA256Hash
sha256(x23::is_bytespan auto buf)
{
  SHA256Hash out;
  [[maybe_unused]]
  int ret = crypto_hash_sha256(out.data(),
                               reinterpret_cast<const uint8_t *>(buf.data()),
                               buf.size());
  // this libsodium call cannot fail (V1.20).
  assert(ret == 0);
  return out;
}

template<x23::is_xdr_type T> requires (!x23::is_bytespan<T>)
inline SHA256Hash
sha256(const T &value)
{
  auto buf = x23::serialize(value);
  return sha256(buf);
}

// SipHash24Key is the secret key used by SipHash24.
using SipHash24Key = std::array<uint8_t, 16>;

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
static_assert(sizeof(SipHash24Key) == crypto_shorthash_siphash24_KEYBYTES);
static_assert(sizeof(uint64_t) == crypto_shorthash_siphash24_BYTES);

uint64_t
SipHash24::operator()(x23::is_bytespan auto buf)
{
  uint64_t out;
  [[maybe_unused]]
  int ret =
      crypto_shorthash_siphash24(reinterpret_cast<uint8_t *>(&out),
                                 reinterpret_cast<const uint8_t *>(buf.data()),
                                 buf.size(), key_.data());
  // this libsodium call cannot fail (V1.20).
  assert(ret == 0);
  return out;
}

// ed25519_verify verifies sig for a msg and publickey.
inline bool
ed25519_verify(const Ed25519PublicKey &publickey, const Ed25519Signature &sig,
               x23::is_bytespan auto msg)
{
  return crypto_sign_ed25519_verify_detached(sig.data(),
                                             reinterpret_cast<const uint8_t *>(
                                                 msg.data()),
                                             msg.size(), publickey.data()) == 0;
}

// Used for deterministic loadgen
inline std::pair<Ed25519PublicKey, Ed25519SecretKey>
deterministic_keypair(uint64_t nonce)
{
  Ed25519SecretKey secret_key;
  Ed25519PublicKey public_key;
  uint8_t seed[crypto_sign_ed25519_SEEDBYTES];
  std::memcpy(seed, reinterpret_cast<uint8_t *>(&nonce), sizeof(nonce));
  [[maybe_unused]]
  int res = crypto_sign_ed25519_seed_keypair(public_key.data(),
                                             secret_key.data(), seed);
  assert(res == 0);
  return std::make_pair(public_key, secret_key);
}

inline Ed25519Signature
ed25519_sign(const Ed25519SecretKey &sk, x23::is_bytespan auto msg)
{
  Ed25519Signature out;
  [[maybe_unused]]
  auto res = crypto_sign_ed25519_detached(out.data(), nullptr, msg.data(),
                                          msg.size(), sk.data());
  assert(res == 0);
  return out;
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
