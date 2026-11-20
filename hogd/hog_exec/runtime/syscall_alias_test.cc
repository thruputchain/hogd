#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <tuple>
#include <utility>
#include <vector>

#include "hogd/base/types.h"
#include "hogd/crypto/crypto.h"
#include "hogd/hog_exec/common/status.h"
#include "hogd/hog_exec/precompiles/context.h"
#include "hogd/hog_exec/runtime/gas_cost_table.h"
#include "hogd/hog_exec/runtime/native_contract_test_framework.h"
#include "hogd/hog_exec/runtime/runtime_exports.h"
#include "hogd/hog_exec/runtime/syscall_entry.h"
#include "hogd/hog_exec/runtime/syscall_handler.h"
#include "invoke_flag.hh"
#include "rpc_types.hh"
#include "syscall.hh"
#include "syscall_error.hh"
#include "transaction.hh"
#include "types.hh"

// Which memory arguments of a syscall may overlap, as a guest sees it through
// syscall_entry: inputs may share bytes with each other, and an output may
// share bytes with nothing. AliasGuard's own tests cover the predicate; these
// cover that each kind of argument is registered as what it is, and what a
// guest gets back.

namespace hogd {
namespace {

constexpr GasAmount kPlentyOfGas = 100'000'000;
constexpr uint32_t kNone = UINT32_MAX;
constexpr std::byte kUntouched{0xA5};

// Guest addresses, far enough apart that nothing overlaps unless a test
// makes it.
constexpr uint32_t kA = 0x040;
constexpr uint32_t kB = 0x080;
constexpr uint32_t kC = 0x0c0;
constexpr uint32_t kOut = 0x100;
constexpr size_t kMemoryBytes = 0x140;

using Words = std::array<uint32_t, 7>;
using Bytes32 = std::array<uint8_t, 32>;

class SyscallAliasEntryTest : public ::testing::Test {
  NativeContractTestFramework framework_{
      false, BlockExecutionMode::kBlockExecutionModeProposal, true};

public:
  // One syscall through syscall_entry in a frame of its own.
  SyscallResult call(SyscallNo syscall, std::span<std::byte> memory,
                     Words words, InvokeFlag flag = InvokeFlag::None)
  {
    auto context = make_hog_exec_context();
    auto handler =
        framework_.make_new_system_call_handler(Transaction{}, *context);
    handler.enable_tracing();
    handler.enable_transaction_stats();
    auto frame = handler.start_frame(
        Invocation{.invoked_address =
                       NativeContractTestFramework::make_contract_address(0),
                   .entrypoint = {},
                   .gas_limit = kPlentyOfGas,
                   .calldata = {}},
        std::to_underlying(flag), ValidatedAuthTokens{});
    EXPECT_TRUE(frame.has_value());
    auto result =
        syscall_entry(&handler,
                      ExecutionContext{.memory = memory,
                                       .gas_remaining = kPlentyOfGas},
                      static_cast<uint32_t>(syscall), words[0], words[1],
                      words[2], words[3], words[4], words[5], words[6])();
    const auto *stats = handler.get_transaction_stats();
    EXPECT_EQ(stats->invalid_syscall_count, 0);
    faults_ = stats->syscall_fault_count;
    EXPECT_TRUE(
        handler.finish_frame(InvokeStatus<void>{}, nullptr)().has_value());
    return result;
  }

  // Syscall faults counted by the last call().
  uint16_t faults() const { return faults_; }

private:
  uint16_t faults_ = 0;
};

std::vector<std::byte>
guest_memory()
{
  return std::vector<std::byte>(kMemoryBytes, kUntouched);
}

// Thirty-two distinct bytes starting at `first`.
constexpr Bytes32
counting_from(uint8_t first)
{
  Bytes32 bytes{};
  for (size_t i = 0; i < bytes.size(); ++i)
    bytes[i] = static_cast<uint8_t>(first + i);
  return bytes;
}

template<typename Bytes>
void
put(std::span<std::byte> memory, uint32_t address, const Bytes &bytes)
{
  std::ranges::copy(std::as_bytes(std::span(bytes)), memory.begin() + address);
}

Bytes32
get32(std::span<const std::byte> memory, uint32_t address)
{
  Bytes32 bytes{};
  std::ranges::copy(memory.subspan(address, bytes.size()),
                    std::as_writable_bytes(std::span(bytes)).begin());
  return bytes;
}

void
expect_code(const SyscallResult &result, SyscallErrorCode code)
{
  ASSERT_TRUE(result.first.has_value());
  EXPECT_EQ(*result.first, code);
}

// A fault from the argument checks, at the generic charge.
void
expect_fault(const SyscallResult &result)
{
  expect_code(result, SyscallErrorCode::kSyscallErrorCodeFault);
  EXPECT_EQ(result.second, kSyscallBaseCost + kGenericBadSyscallCost);
}

// GetDeployAddress(caller_address?, nonce, contract_hash) -> to_be_deployed
// hashes its inputs, so its output says which bytes each input was read as.
// The reference call gives every input a buffer of its own.
TEST_F(SyscallAliasEntryTest, OneBufferCanBeTwoInputs)
{
  auto reference = guest_memory();
  put(reference, kA, counting_from(1));
  put(reference, kB, counting_from(1));
  expect_code(call(SyscallNo::kGetDeployAddress, reference,
                   {kNone, kA, kB, kOut}),
              SyscallErrorCode::kSyscallErrorCodeNone);

  auto memory = guest_memory();
  put(memory, kA, counting_from(1));
  const auto before = memory;
  // nonce and contract_hash are the same thirty-two bytes.
  expect_code(call(SyscallNo::kGetDeployAddress, memory, {kNone, kA, kA, kOut}),
              SyscallErrorCode::kSyscallErrorCodeNone);
  EXPECT_EQ(faults(), 0);
  EXPECT_EQ(get32(memory, kOut), get32(reference, kOut));

  // Only the output was written.
  auto expected = before;
  put(expected, kOut, get32(reference, kOut));
  EXPECT_EQ(memory, expected);
}

TEST_F(SyscallAliasEntryTest, OneBufferCanBeEveryInput)
{
  auto reference = guest_memory();
  put(reference, kA, counting_from(7));
  put(reference, kB, counting_from(7));
  put(reference, kC, counting_from(7));
  expect_code(call(SyscallNo::kGetDeployAddress, reference, {kA, kB, kC, kOut}),
              SyscallErrorCode::kSyscallErrorCodeNone);

  auto memory = guest_memory();
  put(memory, kA, counting_from(7));
  // caller_address (an optional input), nonce and contract_hash together.
  expect_code(call(SyscallNo::kGetDeployAddress, memory, {kA, kA, kA, kOut}),
              SyscallErrorCode::kSyscallErrorCodeNone);
  EXPECT_EQ(faults(), 0);
  EXPECT_EQ(get32(memory, kOut), get32(reference, kOut));
}

TEST_F(SyscallAliasEntryTest, InputsCanOverlapInPart)
{
  // nonce at kA and contract_hash sixteen bytes into it: the hash's first
  // half is the nonce's second half.
  auto memory = guest_memory();
  put(memory, kA, counting_from(1));
  put(memory, kA + 32, counting_from(33));
  expect_code(call(SyscallNo::kGetDeployAddress, memory,
                   {kNone, kA, kA + 16, kOut}),
              SyscallErrorCode::kSyscallErrorCodeNone);
  EXPECT_EQ(faults(), 0);

  auto reference = guest_memory();
  put(reference, kA, counting_from(1));
  put(reference, kB, counting_from(17));
  expect_code(call(SyscallNo::kGetDeployAddress, reference,
                   {kNone, kA, kB, kOut}),
              SyscallErrorCode::kSyscallErrorCodeNone);
  EXPECT_EQ(get32(memory, kOut), get32(reference, kOut));
}

// VerifySignature(scheme, signature, public_key, message) takes three input
// spans and succeeds only if each was read as the bytes it names. The key
// below signs messages cut from its own public key, so that the message span
// can share the public key's bytes.
class SignedByKey {
public:
  SignedByKey()
  {
    const std::array<std::byte, 32> seed{std::byte{7}};
    std::tie(public_key_, secret_key_) = ed25519_keygen_from_seed(seed);
  }

  const Ed25519PublicKey &public_key() const { return public_key_; }

  // A signature on `length` bytes of the public key from `offset`.
  Ed25519Signature sign_slice(size_t offset, size_t length) const
  {
    const auto message =
        std::as_bytes(std::span(public_key_)).subspan(offset, length);
    const auto signature = ed25519_sign(secret_key_, message);
    if (!signature.has_value()) {
      ADD_FAILURE() << "signing failed";
      return {};
    }
    return *signature;
  }

private:
  Ed25519PublicKey public_key_{};
  Ed25519SecretKey secret_key_{};
};

constexpr auto kEd25519 =
    static_cast<uint32_t>(std::to_underlying(CryptographicScheme::Ed25519));

// The public key at kA and a signature at kB; the message is `length` bytes
// at `message`. Returns the syscall's result.
SyscallResult
verify_slice(SyscallAliasEntryTest &test, const SignedByKey &key,
             const Ed25519Signature &signature, uint32_t message,
             uint32_t length)
{
  auto memory = guest_memory();
  put(memory, kA, key.public_key());
  put(memory, kB, signature);
  const auto before = memory;
  const auto result = test.call(SyscallNo::kVerifySignature, memory,
                                {kEd25519, kB, sizeof(Ed25519Signature), kA,
                                 sizeof(Ed25519PublicKey), message, length});
  EXPECT_EQ(memory, before);
  return result;
}

TEST_F(SyscallAliasEntryTest, OneBufferCanBeTwoInputSpans)
{
  const SignedByKey key;
  // The message is the whole public key.
  const auto result = verify_slice(*this, key, key.sign_slice(0, 32), kA, 32);
  expect_code(result, SyscallErrorCode::kSyscallErrorCodeNone);
  EXPECT_EQ(result.second, kSyscallBaseCost + GasCost::verify_signature(32));
  EXPECT_EQ(faults(), 0);
}

TEST_F(SyscallAliasEntryTest, AnInputSpanCanLieInsideAnother)
{
  const SignedByKey key;
  // The message is the public key's middle sixteen bytes.
  const auto result =
      verify_slice(*this, key, key.sign_slice(8, 16), kA + 8, 16);
  expect_code(result, SyscallErrorCode::kSyscallErrorCodeNone);
  EXPECT_EQ(faults(), 0);

  // The same signature does not verify the sixteen bytes one further along,
  // so which bytes the span named was not ignored.
  const auto shifted =
      verify_slice(*this, key, key.sign_slice(8, 16), kA + 9, 16);
  ASSERT_TRUE(shifted.first.has_value());
  EXPECT_NE(*shifted.first, SyscallErrorCode::kSyscallErrorCodeNone);
  EXPECT_NE(*shifted.first, SyscallErrorCode::kSyscallErrorCodeFault);
  EXPECT_EQ(faults(), 0);
}

// A zero-length span strictly inside another range overlaps it under the
// guard's predicate. Between two inputs that no longer matters.
TEST_F(SyscallAliasEntryTest, AZeroLengthInputSpanCanLieInsideAnother)
{
  const SignedByKey key;
  const auto result = verify_slice(*this, key, key.sign_slice(8, 0), kA + 8, 0);
  expect_code(result, SyscallErrorCode::kSyscallErrorCodeNone);
  EXPECT_EQ(faults(), 0);
}

// An output may still overlap nothing: not an input, by one byte or
// entirely.
TEST_F(SyscallAliasEntryTest, AnOutputCannotOverlapAnInput)
{
  for (const Words &words : {
           // to_be_deployed on the nonce, then on each of its end bytes.
           Words{kNone, kA, kB, kA},
           Words{kNone, kA, kB, kA + 31},
           Words{kNone, kA, kB, kA - 31},
           // On the contract hash, and on the optional caller address.
           Words{kNone, kA, kB, kB},
           Words{kC, kA, kB, kC + 31},
           // On an input that two arguments share.
           Words{kNone, kA, kA, kA + 16},
       }) {
    auto memory = guest_memory();
    const auto before = memory;
    expect_fault(call(SyscallNo::kGetDeployAddress, memory, words));
    EXPECT_EQ(faults(), 1);
    EXPECT_EQ(memory, before);
  }
}

// GetCalldataLen() -> len, auth_token_count: two four-byte outputs.
TEST_F(SyscallAliasEntryTest, AnOutputCannotOverlapAnotherOutput)
{
  for (const Words &words :
       {Words{kOut, kOut}, Words{kOut, kOut + 3}, Words{kOut + 3, kOut}}) {
    auto memory = guest_memory();
    const auto before = memory;
    expect_fault(call(SyscallNo::kGetCalldataLen, memory, words));
    EXPECT_EQ(faults(), 1);
    EXPECT_EQ(memory, before);
  }

  // Back to back is not overlap.
  auto memory = guest_memory();
  expect_code(call(SyscallNo::kGetCalldataLen, memory, {kOut, kOut + 4}),
              SyscallErrorCode::kSyscallErrorCodeNone);
  EXPECT_EQ(faults(), 0);
}

// SHA256(buffer) -> hash. A zero-length input span strictly inside the output
// still overlaps it; at either edge of the output it does not.
TEST_F(SyscallAliasEntryTest, ZeroLengthInputSpanAgainstAnOutput)
{
  {
    auto memory = guest_memory();
    const auto before = memory;
    expect_fault(call(SyscallNo::kSHA256, memory, {kOut + 8, 0, kOut}));
    EXPECT_EQ(faults(), 1);
    EXPECT_EQ(memory, before);
  }
  const SHA256Hash empty_hash = sha256(std::span<const std::byte>{});
  for (const uint32_t edge : {kOut, kOut + 32}) {
    auto memory = guest_memory();
    expect_code(call(SyscallNo::kSHA256, memory, {edge, 0, kOut}),
                SyscallErrorCode::kSyscallErrorCodeNone);
    EXPECT_EQ(faults(), 0);
    EXPECT_TRUE(std::ranges::equal(get32(memory, kOut), empty_hash));
  }
}

// BytestringGet(key) -> bytestring, written_len. The same rule with the
// zero-length span as the output and the range around it an input.
TEST_F(SyscallAliasEntryTest, ZeroLengthOutputSpanAgainstAnInput)
{
  {
    auto memory = guest_memory();
    const auto before = memory;
    expect_fault(
        call(SyscallNo::kBytestringGet, memory, {kA, kA + 8, 0, kOut}));
    EXPECT_EQ(faults(), 1);
    EXPECT_EQ(memory, before);
  }
  for (const uint32_t edge : {kA, kA + 32}) {
    auto memory = guest_memory();
    const auto result =
        call(SyscallNo::kBytestringGet, memory, {kA, edge, 0, kOut});
    // The arguments pass; what the body then says about a key nobody wrote
    // is not this test's business.
    ASSERT_TRUE(result.first.has_value());
    EXPECT_NE(*result.first, SyscallErrorCode::kSyscallErrorCodeFault);
    EXPECT_EQ(faults(), 0);
  }
}

// The argument checks come before the read-only check, so in a read-only
// frame an overlap involving an output is still a fault, and the same call
// without the overlap is refused as a write. RetrocausalQueuePeekLen(key) ->
// len is a write with an output to overlap.
TEST_F(SyscallAliasEntryTest, AnOverlapFaultsBeforeTheReadOnlyCheck)
{
  {
    auto memory = guest_memory();
    const auto before = memory;
    expect_fault(call(SyscallNo::kRetrocausalQueuePeekLen, memory, {kA, kA + 8},
                      InvokeFlag::ReadOnly));
    EXPECT_EQ(faults(), 1);
    EXPECT_EQ(memory, before);
  }
  {
    auto memory = guest_memory();
    const auto before = memory;
    const auto result = call(SyscallNo::kRetrocausalQueuePeekLen, memory,
                             {kA, kOut}, InvokeFlag::ReadOnly);
    expect_code(result, SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput);
    EXPECT_EQ(result.second, kSyscallBaseCost);
    EXPECT_EQ(faults(), 0);
    EXPECT_EQ(memory, before);
  }
}

// BytestringSet(key, bytestring) has only inputs, so overlapping them is no
// longer a fault for the read-only check to come after: a read-only frame is
// refused as a write whether or not they overlap.
TEST_F(SyscallAliasEntryTest, OverlappingInputsReachTheReadOnlyCheck)
{
  for (const Words &words :
       {Words{kA, kA + 8, 0}, Words{kA, kA + 8, 16}, Words{kA, kB, 16}}) {
    auto memory = guest_memory();
    const auto before = memory;
    const auto result =
        call(SyscallNo::kBytestringSet, memory, words, InvokeFlag::ReadOnly);
    expect_code(result, SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput);
    EXPECT_EQ(result.second, kSyscallBaseCost);
    EXPECT_EQ(faults(), 0);
    EXPECT_EQ(memory, before);
  }
}

} // namespace
} // namespace hogd
