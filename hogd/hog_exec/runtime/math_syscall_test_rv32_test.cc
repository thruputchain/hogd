#include <gtest/gtest.h>

// End-to-end rv32 coverage of the math syscalls, driven through the
// math_syscall_test contract (local_test_contracts): a real guest makes each
// call and either returns what it got or asserts on it itself.

#include <array>
#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <utility>

#include "built_contracts/math_syscall_test.h"
#include "hogd/base/network_config_params.h"
#include "hogd/base/types.h"
#include "hogd/hog_exec/precompiles/context.h"
#include "hogd/hog_exec/runtime/contract_trait.h"
#include "hogd/hog_exec/runtime/contract_trait_client.h"
#include "hogd/hog_exec/runtime/native_contract_test_framework.h"
#include "hogd/hog_exec/runtime/runtime.h"
#include "rpc_types.hh"
#include "transaction.hh"
#include "types.hh"

namespace hogd {

namespace {

// A 256-bit value as a syscall takes it: 32 bytes, least significant first.
// The words are given most significant first, the way the number is read.
constexpr std::array<uint8_t, 32>
u256_le(uint64_t word3, uint64_t word2, uint64_t word1, uint64_t word0)
{
  const std::array<uint64_t, 4> words = {word0, word1, word2, word3};
  std::array<uint8_t, 32> bytes{};
  for (size_t i = 0; i < bytes.size(); ++i) {
    bytes[i] = static_cast<uint8_t>(words[i / 8] >> (8 * (i % 8)));
  }
  return bytes;
}

constexpr uint128_t
u128(uint64_t high, uint64_t low)
{
  return (uint128_t{high} << 64) | low;
}

} // namespace

// NOLINTNEXTLINE(misc-use-internal-linkage)
class MathSyscallTestRv32Test : public ::testing::Test,
                                public NativeContractTestFramework {
public:
  MathSyscallTestRv32Test()
    : NativeContractTestFramework(
          false, BlockExecutionMode::kBlockExecutionModeProposal)
  {}

protected:
  using math_syscall_test_meta =
      ContractTraitMeta<math_syscall_test::MathSyscallTest>;

  void SetUp() override
  {
    auto contract = math_syscall_test_meta::load_rv32();
    write_contract(contract);
    write_deployment(kContractAddress, contract);
    setup_grant_storage_token(kContractAddress, 1'000'000'000);

    runtime_context_ = make_hog_exec_context();
  }

  static constexpr ContractAddress kContractAddress = make_contract_address(0);

  // The root a guest gets from U256Sqrt for `value`, or nothing, with the
  // failure recorded, if the call does not return one.
  std::optional<uint128_t> u256_sqrt(const std::array<uint8_t, 32> &value)
  {
    auto handler = make_new_system_call_handler(
        Transaction{.invocation =
                        client_.make_invocation<u8"u256_sqrt">(kDefaultMaxTxGas,
                                                               value),
                    .uniquifier = make_new_uniquifier()},
        *runtime_context_);
    handler.enable_transaction_stats();
    if (!handler.root_invoke()().is_root_success()) {
      ADD_FAILURE() << "u256_sqrt did not succeed";
      return std::nullopt;
    }
    const auto *stats = handler.get_transaction_stats();
    if (stats == nullptr || stats->syscall_fault_count != 0) {
      ADD_FAILURE() << "u256_sqrt made a syscall that faulted";
      return std::nullopt;
    }

    auto output = handler.get_most_recent_frame_output();
    if (output.status_code() != InvocationFrameStatusCode::Return) {
      ADD_FAILURE() << "u256_sqrt did not return";
      return std::nullopt;
    }
    const auto decoded = math_syscall_test_meta::decode_return<u8"u256_sqrt">(
        std::as_bytes(std::span(output.return_buffer().data(),
                                output.return_buffer().size())),
        false);
    if (!decoded.has_value()) {
      ADD_FAILURE() << "u256_sqrt returned something that is not a root";
      return std::nullopt;
    }
    uint128_t root = 0;
    for (size_t i = 0; i < decoded->size(); ++i) {
      root |= uint128_t{(*decoded)[i]} << (8 * i);
    }
    return root;
  }

  // Runs one contract method as a transaction of its own. The method asserts
  // on every syscall result itself, so it returning is the pass; `faults` is
  // how many of its syscalls it expects to fault.
  void expect_method_passes(Invocation invocation, uint16_t faults)
  {
    auto handler = make_new_system_call_handler(
        Transaction{.invocation = std::move(invocation),
                    .uniquifier = make_new_uniquifier()},
        *runtime_context_);
    handler.enable_transaction_stats();
    ASSERT_TRUE(handler.root_invoke()().is_root_success());

    const auto *stats = handler.get_transaction_stats();
    ASSERT_NE(stats, nullptr);
    EXPECT_EQ(stats->syscall_fault_count, faults);
  }

  ContractTraitClient<math_syscall_test::MathSyscallTest> client_ =
      ContractTraitClient<math_syscall_test::MathSyscallTest>(kContractAddress);

  std::unique_ptr<RuntimeContext> runtime_context_;
};

// The roots here were worked out apart from the node, so they hold the byte
// order as well as the arithmetic: a value read most significant byte first,
// or a root written that way, gets none of the wide ones right.
TEST_F(MathSyscallTestRv32Test, U256SqrtRoots)
{
  EXPECT_EQ(u256_sqrt(u256_le(0, 0, 0, 0)), u128(0, 0));
  EXPECT_EQ(u256_sqrt(u256_le(0, 0, 0, 1)), u128(0, 1));
  EXPECT_EQ(u256_sqrt(u256_le(0, 0, 0, 3)), u128(0, 1));
  EXPECT_EQ(u256_sqrt(u256_le(0, 0, 0, 4)), u128(0, 2));

  // 2^128 -> 2^64: one set bit in, one set bit out, in different words.
  EXPECT_EQ(u256_sqrt(u256_le(0, 1, 0, 0)), u128(1, 0));
  // 2^255 -> floor(2^127.5): the digits of the square root of two.
  EXPECT_EQ(u256_sqrt(u256_le(0x8000'0000'0000'0000, 0, 0, 0)),
            u128(0xb504'f333'f9de'6484, 0x597d'89b3'754a'be9f));
  // The largest value has the largest root.
  EXPECT_EQ(u256_sqrt(u256_le(~uint64_t{0}, ~uint64_t{0}, ~uint64_t{0},
                              ~uint64_t{0})),
            u128(~uint64_t{0}, ~uint64_t{0}));

  // The square of 0xfedcba98765432100123456789abcdef, and the value below
  // it, whose root is one less.
  EXPECT_EQ(u256_sqrt(u256_le(0xfdba'c097'c8dc'5acc, 0xe130'60d8'ff39'ef84,
                              0x446e'fc86'a6f7'108c, 0xdca5'e208'90f2'a521)),
            u128(0xfedc'ba98'7654'3210, 0x0123'4567'89ab'cdef));
  EXPECT_EQ(u256_sqrt(u256_le(0xfdba'c097'c8dc'5acc, 0xe130'60d8'ff39'ef84,
                              0x446e'fc86'a6f7'108c, 0xdca5'e208'90f2'a520)),
            u128(0xfedc'ba98'7654'3210, 0x0123'4567'89ab'cdee));
}

// The guest checks the syscall against the SDK's software square root, which
// it stands in for, and against the definition of a floor square root.
TEST_F(MathSyscallTestRv32Test, U256SqrtMatchesSoftware)
{
  expect_method_passes(
      client_.make_invocation<u8"test_u256_sqrt_matches_software">(
          kDefaultMaxTxGas),
      0);
}

// A root cannot be computed in place: four placements of the root on the
// value fault, and the two that only touch it do not.
TEST_F(MathSyscallTestRv32Test, U256SqrtRootMayNotOverlapValue)
{
  expect_method_passes(
      client_.make_invocation<u8"test_u256_sqrt_root_may_not_overlap_value">(
          kDefaultMaxTxGas),
      4);
}

} // namespace hogd
