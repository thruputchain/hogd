// crypto.h - cryptographic utilities

#pragma once

#include <cstdlib>
extern "C" {
#include "blake3.h"
}

#include <openssl/rand.h> // NOLINT(misc-include-cleaner)
#include <openssl/sha.h>  // NOLINT(misc-include-cleaner)

#include <crc32c/crc32c.h>

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdio>
#include <optional>
#include <span>
#include <utility>
#include <vector>
#include <version>

#include "x23/serialize.h"
#include "x23/vec.h"
#include "x23/x23.h"

#include "hogd/base/compiler.h"
#include "hogd/base/types.h"
#include "hogd/rust/crypto/bindings.h"
#include "types.hh"

namespace hogd {

class SHA256Hasher {
public:
  SHA256Hasher()
  {
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
    SHA256_Init(&ctx_);
  }

  void update(std::span<const std::byte> buf)
  {
    if (buf.empty()) {
      return;
    }
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
    SHA256_Update(&ctx_, buf.data(), buf.size());
  }

  void update(x23::BufView buf)
  {
    if (buf.empty()) {
      return;
    }
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
    SHA256_Update(&ctx_, buf.data(), buf.size());
  }

  SHA256Hash finalize()
  {
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
    SHA256Hash out;
    SHA256_Final(out.data(), &ctx_);
    return out;
  }

private:
  SHA256_CTX ctx_;
};

// read_random fills buf with high-quality random bytes (e.g., /dev/urandom)
// suitable for creating secret keys.
inline void
read_random(std::span<std::byte> buf)
{
  if (RAND_bytes(reinterpret_cast<uint8_t *>(buf.data()), buf.size()) != 1) {
    perror("Failed to read random bytes");
    exit(EXIT_FAILURE);
  }
}

// sha256 returns the SHA256 hash of buf.
inline SHA256Hash
sha256(x23::BufView buf)
{
  SHA256Hasher hasher;
  hasher.update(buf);
  return hasher.finalize();
}

template<x23::is_xdr_type T> requires (!x23::is_bytespan<T>)
inline SHA256Hash
sha256(const T &value)
{
  return sha256(x23::serialize(value));
}

using Blake3Hash = std::array<unsigned char, BLAKE3_OUT_LEN>;

inline Blake3Hash
blake3(x23::BufView buf)
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

namespace detail {

template<size_t N>
auto
span_to_pointer(std::span<const std::byte, N> span)
{
  return reinterpret_cast<const uint8_t (*)[N]>(span.data());
}

template<size_t N>
auto
span_to_pointer(std::array<uint8_t, N> const &data)
{
  return span_to_pointer(std::as_bytes(std::span(data)));
}

template<size_t N>
auto
span_to_writable_pointer(std::array<uint8_t, N> &data)
{
  return reinterpret_cast<uint8_t (*)[N]>(data.data());
}

inline MessageSpan
span_to_message_span(std::span<const std::byte> msg)
{
  return MessageSpan{.message_ptr =
                         reinterpret_cast<const uint8_t *>(msg.data()),
                     .message_len = msg.size()};
}

inline MessageSpan
span_to_message_span(x23::BufView msg)
{
  return MessageSpan{.message_ptr =
                         reinterpret_cast<const uint8_t *>(msg.data()),
                     .message_len = msg.size()};
}

} // namespace detail

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
  uint64_t operator()(x23::BufView buf);

  SipHash24Key get_key() const { return key_; }

private:
  // the secret key.
  SipHash24Key key_;
};

inline uint64_t
SipHash24::operator()(x23::BufView buf)
{
  return siphash24_hash(key_[0], key_[1], detail::span_to_message_span(buf));
}

// SipHash24Hasher is a drop-in unordered_map/unordered_set hasher for fixed
// byte arrays (ContractAddress, SHA256Hash, etc.) that uses the process-wide
// keyed SipHash24, so adversaries can't pack containers full of bucket-
// colliding keys.
struct SipHash24Hasher {
  template<std::size_t N>
  std::size_t operator()(const std::array<uint8_t, N> &array) const
  {
    return SipHash24::get_default()(std::as_bytes(std::span(array)));
  }
};

// ed25519_keygen generates a new ed25519 keypair.
inline std::pair<Ed25519PublicKey, Ed25519SecretKey>
ed25519_keygen()
{
  Ed25519PublicKey pk;
  Ed25519SecretKey sk;
  static_assert(sizeof(Ed25519PublicKey) == ED25519_PUBLIC_KEY_SIZE);
  static_assert(sizeof(Ed25519SecretKey) == ED25519_SECRET_KEYPAIR_SIZE);
  std::array<uint8_t, 32> random_bytes;
  read_random(std::as_writable_bytes(std::span(random_bytes)));
  ed25519_dalek_keygen(detail::span_to_pointer(random_bytes),
                       detail::span_to_writable_pointer(pk),
                       detail::span_to_writable_pointer(sk));
  return {pk, sk};
}

// ed25519_keygen_from_seed deterministically derives an ed25519 keypair from
// a 32-byte seed.
inline std::pair<Ed25519PublicKey, Ed25519SecretKey>
ed25519_keygen_from_seed(std::span<const std::byte, 32> seed)
{
  Ed25519PublicKey pk;
  Ed25519SecretKey sk;
  static_assert(sizeof(Ed25519PublicKey) == ED25519_PUBLIC_KEY_SIZE);
  static_assert(sizeof(Ed25519SecretKey) == ED25519_SECRET_KEYPAIR_SIZE);
  ed25519_dalek_keygen(detail::span_to_pointer(seed),
                       detail::span_to_writable_pointer(pk),
                       detail::span_to_writable_pointer(sk));
  return {pk, sk};
}

template<x23::is_bytespan T>
inline bool
ed25519_verify_batch(std::span<const Ed25519PublicKey> public_keys,
                     std::span<const Ed25519Signature> signatures,
                     std::span<const T> msgs)
{
  if (public_keys.size() != signatures.size() ||
      public_keys.size() != msgs.size()) {
    return false;
  }
  using pk_ptr = const uint8_t (*)[32];
  using sig_ptr = const uint8_t (*)[64];
  std::vector<pk_ptr> public_keys_ptr(public_keys.size());
  std::vector<sig_ptr> signatures_ptr(public_keys.size());
  std::vector<MessageSpan> msgs_ptr(public_keys.size());
  for (size_t i = 0; i < public_keys.size(); i++) {
    public_keys_ptr[i] = detail::span_to_pointer(public_keys[i]);
    signatures_ptr[i] = detail::span_to_pointer(signatures[i]);
    msgs_ptr[i] = detail::span_to_message_span(msgs[i]);
  }
  auto result =
      ed25519_dalek_verify_batch(public_keys_ptr.data(), signatures_ptr.data(),
                                 msgs_ptr.data(), public_keys.size());
  return result == Ed25519Result::Success;
}

inline bool
ed25519_verify(std::span<const std::byte, sizeof(Ed25519PublicKey)> public_key,
               std::span<const std::byte, sizeof(Ed25519Signature)> signature,
               std::span<const std::byte> msg)
{
  return ed25519_dalek_verify(detail::span_to_pointer(public_key),
                              detail::span_to_pointer(signature),
                              detail::span_to_message_span(msg)) ==
         Ed25519Result::Success;
}

// ed25519_verify_rust verifies sig for a msg and publickey using Rust
// implementation.
inline bool
ed25519_verify(const Ed25519PublicKey &publickey, const Ed25519Signature &sig,
               x23::BufView msg)
{
  return ed25519_dalek_verify(detail::span_to_pointer(publickey),
                              detail::span_to_pointer(sig),
                              detail::span_to_message_span(std::as_bytes(
                                  std::span(msg.data(), msg.size())))) ==
         Ed25519Result::Success;
}

inline std::optional<Ed25519Signature>
ed25519_sign(const Ed25519SecretKey &sk, x23::BufView msg)
{
  Ed25519Signature sig{};
  uint8_t ret = ed25519_dalek_sign(detail::span_to_pointer(sk),
                                   detail::span_to_message_span(msg),
                                   detail::span_to_writable_pointer(sig));
  if (ret != 0) {
    return std::nullopt;
  }
  return sig;
}

// crc32c calculates the CRC32C of buf.
inline uint32_t
crc32c(x23::BufView buf)
{
  return crc32c::Crc32c(reinterpret_cast<const uint8_t *>(buf.data()),
                        buf.size());
}

// crc32c_append extends a previous CRC32C to include buf.
inline uint32_t
crc32c_append(uint32_t crc, x23::BufView buf)
{
  return crc32c::Extend(crc, reinterpret_cast<const uint8_t *>(buf.data()),
                        buf.size());
}

} // namespace hogd
