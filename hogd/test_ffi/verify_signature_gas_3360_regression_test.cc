// Regression test for issue #3360.
//
// syscall_VerifySignature_inner() used to return WrappedGasAmount{0} on every
// path, so a contract could force full host-side Ed25519 verification -- fixed
// elliptic-curve work plus a SHA-512 pass over an attacker-sized message --
// while the syscall reported zero host gas. The fix charges
// Ed25519VerifyCost(message_len) = base + per-byte on every path that actually
// runs the verifier, and leaves the pre-verifier size-mismatch path free.
//
// These tests drive the real kVerifySignature syscall through the production
// wrap_after_parse() gas epilogue via the mock frame harness (the same harness
// mock_frame_parent_gas_2725_regression_test.cc uses for kSHA256), then read
// the frame's metered gas back out.

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "hogd/base/types.h"
#include "hogd/crypto/crypto.h"
#include "hogd/hog_exec/runtime/gas_cost_table.h"
#include "hogd/test_ffi/test_bindings.h"
#include "hogd/test_ffi/test_ffi_test_helpers.h"
#include "syscall.hh"
#include "syscall_error.hh"
#include "types.hh"
#include "x23/cleanup.h"

namespace hogd {
namespace {

// A budget far larger than any charge here, so the gas epilogue never rewrites
// a metered syscall to OutOfGas and the frame reports its exact spend.
constexpr GasAmount kFrameGas = 100'000'000;

struct VerifyOutcome {
  int32_t result_code;
  uint64_t gas_consumed;
};

// Stand up a fresh mocked handler, run exactly one kVerifySignature syscall
// against a single frame with the given (already sized) argument spans, and
// return the syscall's result code together with the frame's metered gas.
VerifyOutcome
run_one_verify(std::span<const std::byte> signature,
               std::span<const std::byte> public_key,
               std::span<const std::byte> message)
{
  VerifyOutcome outcome{};

  auto *db = make_db();
  EXPECT_NE(db, nullptr);
  x23::Defer d([db] { free_db(db); });

  auto *rc = make_runtime_context();
  EXPECT_NE(rc, nullptr);
  x23::Defer d2([rc] { close_runtime_context(rc); });

  auto handler = make_mocked_handler(db, rc);
  EXPECT_EQ(handler.ffi_error_code, kTestFFIErrorCodeNone);
  EXPECT_NE(handler.syscall_handler, nullptr);

  Invocation invocation{
      .invoked_address = ContractAddress{0xA0},
      .entrypoint = entrypoint_from_method_id(0x1111),
      .gas_limit = kFrameGas,
      .calldata = {},
  };
  EXPECT_EQ(open_frame(handler.syscall_handler, invocation),
            kTestFFIErrorCodeNone);
  EXPECT_EQ(push_context(handler.syscall_handler), kTestFFIErrorCodeNone);

  auto res = mock_syscall_entrypoint(
      std::to_underlying(SyscallNo::kVerifySignature),
      static_cast<uint64_t>(std::to_underlying(CryptographicScheme::Ed25519)),
      reinterpret_cast<uint64_t>(signature.data()), signature.size(),
      reinterpret_cast<uint64_t>(public_key.data()), public_key.size(),
      reinterpret_cast<uint64_t>(message.data()), message.size());
  outcome.result_code = res.result_code;

  EXPECT_EQ(pop_context(), kTestFFIErrorCodeNone);

  auto finish =
      finish_mock_invocation_frame(handler.syscall_handler,
                                   std::to_underlying(InvokeErrorCode::None));
  EXPECT_EQ(finish.ffi_error_code, kTestFFIErrorCodeNone);
  outcome.gas_consumed = finish.gas_consumed;
  free_ffi_data_buffer(finish.serialized_output);
  free_ffi_data_buffer(finish.serialized_trace);
  free_ffi_data_buffer(finish.serialized_events);

  EXPECT_EQ(close_syscall_handler(handler.syscall_handler),
            kTestFFIErrorCodeNone);
  return outcome;
}

// A successful Ed25519 verification is metered as base + per-byte over the
// message, on the same production path a contract would hit -- so the cost
// scales with the attacker-controlled message length rather than being free.
TEST(VerifySignatureGas, SuccessfulVerifyIsMeteredByMessageLength)
{
  auto [public_key, secret_key] = ed25519_keygen();

  std::array<std::byte, sizeof(Ed25519PublicKey)> public_key_bytes{};
  std::memcpy(public_key_bytes.data(), std::addressof(public_key),
              public_key_bytes.size());

  for (size_t message_len : {size_t{32}, size_t{512}}) {
    std::vector<std::byte> message(message_len, std::byte{0x5a});
    auto signature =
        ed25519_sign(secret_key, std::as_bytes(std::span(message)));
    ASSERT_TRUE(signature.has_value());

    std::array<std::byte, sizeof(Ed25519Signature)> signature_bytes{};
    std::memcpy(signature_bytes.data(), std::addressof(*signature),
                signature_bytes.size());

    auto outcome = run_one_verify(signature_bytes, public_key_bytes, message);

    EXPECT_EQ(outcome.result_code,
              std::to_underlying(SyscallErrorCode::kSyscallErrorCodeNone));
    EXPECT_EQ(outcome.gas_consumed, Ed25519VerifyCost(message_len))
        << "message_len=" << message_len;
    EXPECT_GT(outcome.gas_consumed, 0U);
  }
}

// The pre-verifier size checks must stay free: a wrong-sized public key is
// rejected before any cryptographic work, so it is not metered. This pins the
// fix to the verify path and keeps size-mismatch rejection from being charged.
TEST(VerifySignatureGas, WrongSizedKeyIsRejectedWithoutCharge)
{
  std::array<std::byte, sizeof(Ed25519Signature)> signature{};
  std::array<std::byte, sizeof(Ed25519PublicKey) - 1> short_key{};
  std::array<std::byte, 128> message{};

  auto outcome = run_one_verify(signature, short_key, message);

  EXPECT_EQ(outcome.result_code,
            std::to_underlying(
                SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput));
  EXPECT_EQ(outcome.gas_consumed, 0U);
}

} // namespace
} // namespace hogd
