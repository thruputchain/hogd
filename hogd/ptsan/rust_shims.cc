// rust_shims.cc - PointerTagSanitizer shims for the Rust crypto crate
//
// See shim.h.

#include <cstdint>
#include <vector>

#include "hogd/ptsan/shim.h"
#include "hogd/rust/crypto/bindings.h"

namespace hogd {

using ptsan::raw;
using ptsan::UninstrumentedCall;

namespace {

MessageSpan
raw(MessageSpan span) noexcept
{
  return {ptsan::raw(span.message_ptr), span.message_len};
}

} // namespace

HOGD_PTSAN_WRAPPED(siphash24_hash);
HOGD_PTSAN_WRAPPED(ed25519_dalek_verify);
HOGD_PTSAN_WRAPPED(ed25519_dalek_verify_batch);
HOGD_PTSAN_WRAPPED(ed25519_dalek_sign);
HOGD_PTSAN_WRAPPED(ed25519_dalek_keygen);
HOGD_PTSAN_WRAPPED(ristretto255_msm);

extern "C" {

uint64_t
__wrap_siphash24_hash(uint64_t key1, uint64_t key2, MessageSpan data)
{
  UninstrumentedCall call;
  return __real_siphash24_hash(key1, key2, raw(data));
}

Ed25519Result
__wrap_ed25519_dalek_verify(const uint8_t (*public_key)[32],
                            const uint8_t (*signature)[64], MessageSpan message)
{
  UninstrumentedCall call;
  return __real_ed25519_dalek_verify(raw(public_key), raw(signature),
                                     raw(message));
}

// The three arrays hold pointers of their own, which the callee follows.
Ed25519Result
__wrap_ed25519_dalek_verify_batch(const uint8_t (*const *public_keys)[32],
                                  const uint8_t (*const *signatures)[64],
                                  const MessageSpan *messages,
                                  uintptr_t num_messages)
{
  UninstrumentedCall call;
  public_keys = raw(public_keys);
  signatures = raw(signatures);
  messages = raw(messages);
  std::vector<const uint8_t (*)[32]> raw_public_keys(num_messages);
  std::vector<const uint8_t (*)[64]> raw_signatures(num_messages);
  std::vector<MessageSpan> raw_messages(num_messages);
  for (uintptr_t i = 0; i < num_messages; i++) {
    raw_public_keys[i] = raw(public_keys[i]);
    raw_signatures[i] = raw(signatures[i]);
    raw_messages[i] = raw(messages[i]);
  }
  return __real_ed25519_dalek_verify_batch(raw_public_keys.data(),
                                           raw_signatures.data(),
                                           raw_messages.data(), num_messages);
}

uint8_t
__wrap_ed25519_dalek_sign(const uint8_t (*secret_keypair)[64],
                          MessageSpan message, uint8_t (*signature_out)[64])
{
  UninstrumentedCall call;
  return __real_ed25519_dalek_sign(raw(secret_keypair), raw(message),
                                   raw(signature_out));
}

void
__wrap_ed25519_dalek_keygen(const uint8_t (*random_bytes)[32],
                            uint8_t (*public_key_out)[32],
                            uint8_t (*secret_keypair_out)[64])
{
  UninstrumentedCall call;
  __real_ed25519_dalek_keygen(raw(random_bytes), raw(public_key_out),
                              raw(secret_keypair_out));
}

bool
__wrap_ristretto255_msm(MessageSpan input, uint8_t (*output)[32])
{
  UninstrumentedCall call;
  return __real_ristretto255_msm(raw(input), raw(output));
}

} // extern "C"

} // namespace hogd
