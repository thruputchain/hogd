// mldsa_oracle - hogd's ML-DSA-65 behind a pipe, for differential testing
//
// The Go test in mldsa_diff/ checks that hogd's ML-DSA-65 agrees with Go's
// crypto/mldsa. It runs this program and writes requests to its standard
// input; each request gets one response on standard output. ctx_len and
// msg_len are 4-byte little-endian lengths.
//
//   'K' seed[32]
//       -> public_key[1952]
//       Derives the key pair for seed (MlDsa65SigningKey::from_seed) and
//       keeps it for 'S'.
//   'S' rnd[32] ctx_len ctx msg_len msg
//       -> 1 sig[3309], or 0 if hogd refuses to sign
//       Signs msg under the context string ctx with the key from the last
//       'K', using rnd as the signing randomness
//       (MlDsa65SigningKey::sign_with_rnd).
//   'V' public_key[1952] sig[3309] ctx_len ctx msg_len msg
//       -> 1 if mldsa65_verify accepts sig, else 0
//
// The program exits with status 0 at the end of its input, and with status 2
// after printing an error for a malformed request.

#include <array>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <print>
#include <span>
#include <vector>

#include "hogd/base/types.h"
#include "hogd/crypto/mldsa.h"
#include "hogd/crypto/mldsa_signer.h"
#include "types.hh"

namespace hogd {
namespace {

// The longest context string or message a request may carry. The Go test
// sends much shorter ones; the limit only stops a corrupt length from
// allocating gigabytes.
constexpr uint32_t kMaxFieldBytes = uint32_t{1} << 24;

[[noreturn]] void
fail(const char *what)
{
  std::println(stderr, "mldsa_oracle: {}", what);
  std::exit(2);
}

void
read_exact(std::span<uint8_t> buf)
{
  if (std::fread(buf.data(), 1, buf.size(), stdin) != buf.size()) {
    fail("truncated request");
  }
}

// read_field reads a length-prefixed byte string.
std::vector<uint8_t>
read_field()
{
  std::array<uint8_t, 4> len_bytes{};
  read_exact(len_bytes);
  uint32_t len = 0;
  for (size_t i = 0; i < len_bytes.size(); ++i) {
    len |= uint32_t{len_bytes[i]} << (8 * i);
  }
  if (len > kMaxFieldBytes) {
    fail("field too long");
  }
  std::vector<uint8_t> field(len);
  read_exact(field);
  return field;
}

void
write_bytes(std::span<const uint8_t> buf)
{
  if (std::fwrite(buf.data(), 1, buf.size(), stdout) != buf.size()) {
    fail("cannot write response");
  }
}

void
write_flag(bool flag)
{
  const std::array<uint8_t, 1> byte{flag ? uint8_t{1} : uint8_t{0}};
  write_bytes(byte);
}

int
serve()
{
  std::optional<MlDsa65SigningKey> key;
  for (int op = std::getchar(); op != EOF; op = std::getchar()) {
    switch (op) {
    case 'K':
    {
      MlDsaSeed seed{};
      read_exact(seed);
      key.emplace(MlDsa65SigningKey::from_seed(seed));
      write_bytes(key->public_key());
      break;
    }
    case 'S':
    {
      if (!key) {
        fail("'S' before 'K'");
      }
      MlDsaRnd rnd{};
      read_exact(rnd);
      const auto ctx = read_field();
      const auto msg = read_field();
      const auto sig = key->sign_with_rnd(msg, ctx, rnd);
      write_flag(sig.has_value());
      if (sig) {
        write_bytes(*sig);
      }
      break;
    }
    case 'V':
    {
      MlDsa65PublicKey public_key{};
      read_exact(public_key);
      MlDsa65Signature sig{};
      read_exact(sig);
      const auto ctx = read_field();
      const auto msg = read_field();
      write_flag(mldsa65_verify(public_key, sig, msg, ctx));
      break;
    }
    default:
      fail("unknown request");
    }
    if (std::fflush(stdout) != 0) {
      fail("cannot write response");
    }
  }
  return EXIT_SUCCESS;
}

} // namespace
} // namespace hogd

// NOLINTBEGIN(bugprone-exception-escape)
int
main()
{
  return hogd::serve();
}
// NOLINTEND(bugprone-exception-escape)
