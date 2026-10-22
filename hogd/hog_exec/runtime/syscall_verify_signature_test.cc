#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <span>
#include <utility>
#include <vector>

#include "ffi_enums.hh"
#include "hogd/base/network_config_params.h"
#include "hogd/base/types.h"
#include "hogd/crypto/crypto.h"
#include "hogd/crypto/mldsa_signer.h"
#include "hogd/hog_exec/common/status.h"
#include "hogd/hog_exec/precompiles/context.h"
#include "hogd/hog_exec/runtime/gas_cost_table.h"
#include "hogd/hog_exec/runtime/native_contract_test_framework.h"
#include "hogd/hog_exec/runtime/runtime_exports.h"
#include "hogd/hog_exec/runtime/syscall_entry_outer.h"
#include "hogd/hog_exec/runtime/syscall_handler.h"
#include "hogd/hog_exec/runtime/syscall_inner.h"
#include "hogd/hog_exec/runtime/syscall_meta.h"
#include "hogd/hog_exec/runtime/syscall_types.h"
#include "invoke_flag.hh"
#include "rpc_types.hh"
#include "syscall.hh"
#include "syscall_error.hh"
#include "transaction.hh"
#include "types.hh"

namespace hogd {
namespace {

constexpr GasAmount kPlentyOfGas = 1'000'000'000;

std::vector<std::byte>
message_of(size_t len)
{
  std::vector<std::byte> message(len);
  for (size_t i = 0; i < len; ++i) {
    message[i] = static_cast<std::byte>((i * 7U) + 1U);
  }
  return message;
}

std::pair<HostFnStatus<SyscallErrorCode>, WrappedGasAmount>
verify(CryptographicScheme scheme, std::span<const std::byte> signature,
       std::span<const std::byte> public_key,
       std::span<const std::byte> message, GasAmount gas = kPlentyOfGas)
{
  return syscall_VerifySignature_inner(WrappedGasAmount{gas},
                                       std::to_underlying(scheme), signature,
                                       public_key, message);
}

void
expect_code(
    const std::pair<HostFnStatus<SyscallErrorCode>, WrappedGasAmount> &result,
    SyscallErrorCode code, GasAmount charged)
{
  ASSERT_TRUE(result.first.has_value());
  EXPECT_EQ(*result.first, code);
  EXPECT_EQ(result.second.get(), charged);
}

class MlDsa65VerifySignatureTest : public ::testing::Test {
protected:
  MlDsa65VerifySignatureTest()
    : key_(MlDsa65SigningKey::from_seed(MlDsaSeed{7}))
  {}

  // A valid signature on message, with the empty context the syscall uses.
  MlDsa65Signature sign(std::span<const std::byte> message) const
  {
    auto signature = key_.sign(message);
    if (!signature.has_value()) {
      ADD_FAILURE() << "signing failed";
      return {};
    }
    return *signature;
  }

  std::span<const std::byte> public_key() const
  {
    return std::as_bytes(std::span(key_.public_key()));
  }

  MlDsa65SigningKey key_;
};

TEST_F(MlDsa65VerifySignatureTest, ValidSignatureVerifiesAtTheEd25519Price)
{
  for (size_t len : {0U, 1U, 70U, 71U, 1024U, 4096U}) {
    SCOPED_TRACE(::testing::Message() << "len=" << len);
    const auto message = message_of(len);
    const auto signature = sign(message);
    expect_code(verify(CryptographicScheme::MlDsa65,
                       std::as_bytes(std::span(signature)), public_key(),
                       message),
                SyscallErrorCode::kSyscallErrorCodeNone,
                GasCost::verify_signature(len));
  }
}

TEST_F(MlDsa65VerifySignatureTest, InvalidSignatureFailsAndIsCharged)
{
  const auto message = message_of(64);
  auto signature = sign(message);
  const GasAmount price = GasCost::verify_signature(message.size());

  auto tampered = signature;
  tampered[0] ^= 1;
  expect_code(verify(CryptographicScheme::MlDsa65,
                     std::as_bytes(std::span(tampered)), public_key(), message),
              SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput, price);

  auto other_message = message;
  other_message[0] ^= std::byte{1};
  expect_code(verify(CryptographicScheme::MlDsa65,
                     std::as_bytes(std::span(signature)), public_key(),
                     other_message),
              SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput, price);

  const auto other_key = MlDsa65SigningKey::from_seed(MlDsaSeed{8});
  expect_code(verify(CryptographicScheme::MlDsa65,
                     std::as_bytes(std::span(signature)),
                     std::as_bytes(std::span(other_key.public_key())), message),
              SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput, price);

  // A malformed signature takes ML-DSA's early exit, and still pays in full.
  MlDsa65Signature malformed{};
  malformed.fill(0xff);
  expect_code(verify(CryptographicScheme::MlDsa65,
                     std::as_bytes(std::span(malformed)), public_key(),
                     message),
              SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput, price);
}

TEST_F(MlDsa65VerifySignatureTest, WrongLengthsAreRejectedForFree)
{
  const auto message = message_of(8);
  const auto signature = sign(message);
  const std::span<const std::byte> sig_bytes =
      std::as_bytes(std::span(signature));
  const auto key_bytes = public_key();

  for (auto [sig, key] :
       {std::pair{sig_bytes.first(sig_bytes.size() - 1), key_bytes},
        std::pair{sig_bytes, key_bytes.first(key_bytes.size() - 1)},
        std::pair{sig_bytes.first(sizeof(Ed25519Signature)),
                  key_bytes.first(sizeof(Ed25519PublicKey))}}) {
    expect_code(verify(CryptographicScheme::MlDsa65, sig, key, message),
                SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput, 0);
  }
  std::vector<std::byte> long_key(key_bytes.begin(), key_bytes.end());
  long_key.push_back(std::byte{0});
  expect_code(verify(CryptographicScheme::MlDsa65, sig_bytes, long_key,
                     message),
              SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput, 0);
}

TEST_F(MlDsa65VerifySignatureTest, RunsOutOfGasBeforeVerifying)
{
  const auto message = message_of(32);
  const auto signature = sign(message);
  const GasAmount price = GasCost::verify_signature(message.size());
  for (GasAmount budget : {GasAmount{0}, price - 1, price}) {
    SCOPED_TRACE(::testing::Message() << "budget=" << budget);
    auto [result, charged] = verify(CryptographicScheme::MlDsa65,
                                    std::as_bytes(std::span(signature)),
                                    public_key(), message, budget);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code(), HostFnErrorCode::OutOfGas);
    EXPECT_EQ(charged.get(), budget);
  }
  expect_code(verify(CryptographicScheme::MlDsa65,
                     std::as_bytes(std::span(signature)), public_key(), message,
                     price + 1),
              SyscallErrorCode::kSyscallErrorCodeNone, price);
}

TEST(Ed25519VerifySignatureTest, PriceIsUnchanged)
{
  EXPECT_EQ(kVerifySignatureBaseCost, 17750U);
  EXPECT_EQ(kVerifySignatureBlockCost, 131U);

  const auto [public_key, secret_key] = ed25519_keygen();
  const auto message = message_of(100);
  const auto signature = ed25519_sign(secret_key, message);
  if (!signature.has_value()) {
    FAIL() << "signing failed";
  }
  expect_code(verify(CryptographicScheme::Ed25519,
                     std::as_bytes(std::span(*signature)),
                     std::as_bytes(std::span(public_key)), message),
              SyscallErrorCode::kSyscallErrorCodeNone,
              GasCost::verify_signature(message.size()));

  // An ML-DSA-sized key under the ed25519 scheme is a length mismatch.
  const std::vector<std::byte> mldsa_sized_key(sizeof(MlDsa65PublicKey));
  expect_code(verify(CryptographicScheme::Ed25519,
                     std::as_bytes(std::span(*signature)), mldsa_sized_key,
                     message),
              SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput, 0);
}

// The whole entry path, guest memory included: the spans reach the inner
// syscall at the lengths ML-DSA-65 needs, and the bill adds only the crossing.
class VerifySignatureEntryTest : public ::testing::Test,
                                 public NativeContractTestFramework {
public:
  VerifySignatureEntryTest()
    : NativeContractTestFramework(
          false, BlockExecutionMode::kBlockExecutionModeProposal,
          TestDB::kHogDb, true)
  {}
};

TEST_F(VerifySignatureEntryTest, MlDsa65ThroughGuestMemory)
{
  auto context = make_hog_exec_context();
  auto handler = make_new_system_call_handler(Transaction{}, *context);
  ASSERT_TRUE(
      handler
          .start_frame(Invocation{.invoked_address = make_contract_address(0),
                                  .entrypoint = {},
                                  .gas_limit = kDefaultMaxTxGas,
                                  .calldata = {}},
                       std::to_underlying(InvokeFlag::None),
                       ValidatedAuthTokens{})
          .has_value());

  const auto key = MlDsa65SigningKey::from_seed(MlDsaSeed{3});
  const auto message = message_of(200);
  const auto signature = key.sign(message);
  ASSERT_TRUE(signature.has_value());

  constexpr uint32_t kSigOffset = 0;
  constexpr uint32_t kKeyOffset = kSigOffset + sizeof(MlDsa65Signature);
  constexpr uint32_t kMessageOffset = kKeyOffset + sizeof(MlDsa65PublicKey);
  const GasAmount cost =
      kSyscallBaseCost + GasCost::verify_signature(message.size());

  for (bool tamper : {false, true}) {
    SCOPED_TRACE(::testing::Message() << "tamper=" << tamper);
    std::vector<std::byte> memory(kMessageOffset + message.size());
    if (signature.has_value()) {
      std::ranges::copy(std::as_bytes(std::span(*signature)),
                        memory.begin() + kSigOffset);
    }
    std::ranges::copy(std::as_bytes(std::span(key.public_key())),
                      memory.begin() + kKeyOffset);
    std::ranges::copy(message, memory.begin() + kMessageOffset);
    if (tamper) {
      memory[kSigOffset] ^= std::byte{1};
    }

    auto [result, charged] =
        wrap_outer<SyscallMeta<SyscallNo::kVerifySignature>>(
            handler,
            ExecutionContext{.memory = memory, .gas_remaining = kPlentyOfGas},
            SyscallNo::kVerifySignature,
            static_cast<uint32_t>(CryptographicScheme::MlDsa65), kSigOffset,
            sizeof(MlDsa65Signature), kKeyOffset, sizeof(MlDsa65PublicKey),
            kMessageOffset, static_cast<uint32_t>(message.size()))();
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result,
              tamper ? SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput
                     : SyscallErrorCode::kSyscallErrorCodeNone);
    EXPECT_EQ(charged, cost);
  }
  EXPECT_TRUE(
      handler.finish_frame(InvokeStatus<void>{}, nullptr)().has_value());
}

} // namespace
} // namespace hogd
