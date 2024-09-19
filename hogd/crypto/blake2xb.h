// Implementation of Blake2xb.
// Specification available at https://www.blake2.net/blake2x.pdf
// This file based on <folly/crypto/blake2xb.{cpp,h}>,
// available at https://github.com/facebook/folly/blob/main/folly/crypto/

// Original source license follows

/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include <array>
#include <bit>
#include <cstdint>
#include <cstring>
#include <span>

#include <sodium.h>

namespace hogd {

namespace detail {

struct Blake2xbParam {
  uint8_t digestLength; /*  1 */
  uint8_t keyLength;    /*  2 */
  uint8_t fanout;       /*  3 */
  uint8_t depth;        /*  4 */
  uint32_t leafLength;  /*  8 */
  uint32_t nodeOffset;  /* 12 */
  uint32_t xofLength;   /* 16 */
  uint8_t nodeDepth;    /* 17 */
  uint8_t innerLength;  /* 18 */
  uint8_t reserved[14]; /* 32 */
  uint8_t salt[16];     /* 48 */
  uint8_t personal[16]; /* 64 */
};

static_assert(sizeof(Blake2xbParam) == 64, "wrong sizeof(Blake2xbParam)");

// In libsodium 1.0.17, the crypto_generichash_blake2b_state struct was made
// opaque. We have to copy the internal definition of the real struct here
// so we can properly initialize it.
struct Blake2bState {
  uint64_t h[8];
  uint64_t t[2];
  uint64_t f[2];
  uint8_t buf[256];
  size_t buflen;
  uint8_t last_node;
};

constexpr std::array<uint64_t, 8> kBlake2bIV = {{
    0x6a09e667f3bcc908ULL,
    0xbb67ae8584caa73bULL,
    0x3c6ef372fe94f82bULL,
    0xa54ff53a5f1d36f1ULL,
    0x510e527fade682d1ULL,
    0x9b05688c2b3e6c1fULL,
    0x1f83d9abfb41bd6bULL,
    0x5be0cd19137e2179ULL,
}};

inline void
init_state_from_params(const Blake2xbParam &param,
                       crypto_generichash_blake2b_state *_state)
{

  auto *state = reinterpret_cast<Blake2bState *>(_state);

  const auto *p = reinterpret_cast<const uint64_t *>(&param);
  for (int i = 0; i < 8; ++i) {
    state->h[i] = kBlake2bIV[i] ^ p[i];
  }

  std::memset(reinterpret_cast<uint8_t *>(state) + sizeof(state->h), 0,
              sizeof(*state) - sizeof(state->h));
}

} // namespace detail

template<uint32_t OutputLength>
void
blake2xb(std::span<const std::byte> input,
         std::span<std::byte, OutputLength> out)
{

  static_assert(std::endian::native == std::endian::little,
                "unimplemented otherwise");

  detail::Blake2xbParam params;

  std::memset(&params, 0, sizeof(params));

  params.digestLength = crypto_generichash_blake2b_BYTES_MAX;
  params.fanout = 1;
  params.depth = 1;
  params.xofLength = OutputLength;

  // all other params 0 (salt, key, personalization unimplemented)

  crypto_generichash_blake2b_state state;

  detail::init_state_from_params(params, &state);

  [[maybe_unused]]
  int res = crypto_generichash_blake2b_update(
      &state, reinterpret_cast<const uint8_t *>(input.data()), input.size());
  assert(res == 0);

  std::array<uint8_t, crypto_generichash_blake2b_BYTES_MAX> h0;
  res = crypto_generichash_blake2b_final(&state, h0.data(), h0.size());
  assert(res == 0);

  // params.kenLength = 0; // already 0, unnecessary
  params.fanout = 0;
  params.depth = 0;
  params.leafLength =
      static_cast<uint32_t>(crypto_generichash_blake2b_BYTES_MAX);
  params.innerLength = crypto_generichash_blake2b_BYTES_MAX;

  size_t pos = 0;
  size_t remaining = out.size();
  while (remaining > 0) {
    params.nodeOffset =
        static_cast<uint32_t>(pos / crypto_generichash_blake2b_BYTES_MAX);
    size_t len =
        std::min(static_cast<size_t>(crypto_generichash_blake2b_BYTES_MAX),
                 remaining);

    params.digestLength = static_cast<uint8_t>(len);

    detail::init_state_from_params(params, &state);
    res = crypto_generichash_blake2b_update(&state, h0.data(), h0.size());
    assert(res == 0);

    res = crypto_generichash_blake2b_final(
        &state, reinterpret_cast<uint8_t *>(out.data()) + pos, len);
    assert(res == 0);

    pos += len;
    remaining -= len;
  }
}

} // namespace hogd
