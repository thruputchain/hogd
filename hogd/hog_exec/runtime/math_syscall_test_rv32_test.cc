#include <gtest/gtest.h>

// End-to-end rv32 coverage of the math syscalls, driven through the
// math_syscall_test contract (local_test_contracts): a real guest makes each
// call and either returns what it got or asserts on it itself.

#include <algorithm>
#include <array>
#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <vector>

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

constexpr uint64_t kOnes = ~uint64_t{0};

// What one MulDivRem call returns.
template<typename T> struct MulDivRemResults {
  T quotient_lo;
  T quotient_hi;
  T remainder;

  bool operator==(const MulDivRemResults &) const = default;
};
using U256Results = MulDivRemResults<std::array<uint8_t, 32>>;
using U128Results = MulDivRemResults<uint128_t>;

// The 16 bytes of `bytes` from `offset`, least significant first, as a value.
template<size_t N>
constexpr uint128_t
u128_at(const std::array<uint8_t, N> &bytes, size_t offset)
{
  uint128_t value = 0;
  for (size_t i = 0; i < sizeof(uint128_t); ++i) {
    value |= uint128_t{bytes[offset + i]} << (8 * i);
  }
  return value;
}

// A 128-bit value as a guest takes it: 16 bytes, least significant first.
constexpr std::array<uint8_t, 16>
u128_le(uint128_t value)
{
  std::array<uint8_t, 16> bytes{};
  for (size_t i = 0; i < bytes.size(); ++i) {
    bytes[i] = static_cast<uint8_t>(value >> (8 * i));
  }
  return bytes;
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

  // What one contract method returned, still encoded, or nothing, with the
  // failure recorded, if it did not return or made a syscall that faulted.
  std::optional<std::vector<std::byte>> returned_by(Invocation invocation)
  {
    auto handler = make_new_system_call_handler(
        Transaction{.invocation = std::move(invocation),
                    .uniquifier = make_new_uniquifier()},
        *runtime_context_);
    handler.enable_transaction_stats();
    if (!handler.root_invoke()().is_root_success()) {
      ADD_FAILURE() << "the call did not succeed";
      return std::nullopt;
    }
    const auto *stats = handler.get_transaction_stats();
    if (stats == nullptr || stats->syscall_fault_count != 0) {
      ADD_FAILURE() << "the call made a syscall that faulted";
      return std::nullopt;
    }

    auto output = handler.get_most_recent_frame_output();
    if (output.status_code() != InvocationFrameStatusCode::Return) {
      ADD_FAILURE() << "the call did not return";
      return std::nullopt;
    }
    const auto bytes = std::as_bytes(std::span(output.return_buffer().data(),
                                               output.return_buffer().size()));
    return std::vector<std::byte>(bytes.begin(), bytes.end());
  }

  // The root a guest gets from U256Sqrt for `value`, or nothing, with the
  // failure recorded, if the call does not return one.
  std::optional<uint128_t> u256_sqrt(const std::array<uint8_t, 32> &value)
  {
    const auto returned = returned_by(
        client_.make_invocation<u8"u256_sqrt">(kDefaultMaxTxGas, value));
    if (!returned.has_value()) {
      return std::nullopt;
    }
    const auto decoded =
        math_syscall_test_meta::decode_return<u8"u256_sqrt">(*returned, false);
    if (!decoded.has_value()) {
      ADD_FAILURE() << "u256_sqrt returned something that is not a root";
      return std::nullopt;
    }
    return u128_at(*decoded, 0);
  }

  // What a guest gets from U256MulDivRem, or nothing, with the failure
  // recorded, if the call does not return three results.
  std::optional<U256Results>
  u256_mul_div_rem(const std::array<uint8_t, 32> &a,
                   const std::array<uint8_t, 32> &b,
                   const std::array<uint8_t, 32> &divisor)
  {
    const auto returned = returned_by(
        client_.make_invocation<u8"u256_mul_div_rem">(kDefaultMaxTxGas, a, b,
                                                      divisor));
    if (!returned.has_value()) {
      return std::nullopt;
    }
    const auto decoded =
        math_syscall_test_meta::decode_return<u8"u256_mul_div_rem">(*returned,
                                                                    false);
    if (!decoded.has_value()) {
      ADD_FAILURE() << "u256_mul_div_rem did not return three results";
      return std::nullopt;
    }
    U256Results results{};
    std::copy_n(decoded->begin(), 32, results.quotient_lo.begin());
    std::copy_n(decoded->begin() + 32, 32, results.quotient_hi.begin());
    std::copy_n(decoded->begin() + 64, 32, results.remainder.begin());
    return results;
  }

  // The same for U128MulDivRem.
  std::optional<U128Results> u128_mul_div_rem(uint128_t a, uint128_t b,
                                              uint128_t divisor)
  {
    const auto returned = returned_by(
        client_.make_invocation<u8"u128_mul_div_rem">(kDefaultMaxTxGas,
                                                      u128_le(a), u128_le(b),
                                                      u128_le(divisor)));
    if (!returned.has_value()) {
      return std::nullopt;
    }
    const auto decoded =
        math_syscall_test_meta::decode_return<u8"u128_mul_div_rem">(*returned,
                                                                    false);
    if (!decoded.has_value()) {
      ADD_FAILURE() << "u128_mul_div_rem did not return three results";
      return std::nullopt;
    }
    return U128Results{.quotient_lo = u128_at(*decoded, 0),
                       .quotient_hi = u128_at(*decoded, 16),
                       .remainder = u128_at(*decoded, 32)};
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

// These results were worked out apart from the node, with Python integers.
// They hold the byte order and the order of the three results as well as the
// arithmetic. The last three sets of operands were found by search: products
// that take the paths of the division that random operands almost never do.
TEST_F(MathSyscallTestRv32Test, U256MulDivRemResults)
{
  // 6 * 7 / 4 is 10, remainder 2.
  EXPECT_EQ(u256_mul_div_rem(u256_le(0, 0, 0, 6), u256_le(0, 0, 0, 7),
                             u256_le(0, 0, 0, 4)),
            (U256Results{u256_le(0, 0, 0, 10), u256_le(0, 0, 0, 0),
                         u256_le(0, 0, 0, 2)}));
  // A quotient that needs both halves:
  // (2^255 + 2^64 + 5) * (2^65 + 1) = (2^256 + 2^191 + 2^65 + 11) * 2^64 + 5
  EXPECT_EQ(u256_mul_div_rem(u256_le(0x8000'0000'0000'0000, 0, 1, 5),
                             u256_le(0, 0, 2, 1), u256_le(0, 0, 1, 0)),
            (U256Results{u256_le(0, 0x8000'0000'0000'0000, 2, 11),
                         u256_le(0, 0, 0, 1), u256_le(0, 0, 0, 5)}));
  // The largest product, undivided: (2^256 - 1)^2 = (2^256 - 2) * 2^256 + 1.
  EXPECT_EQ(u256_mul_div_rem(u256_le(kOnes, kOnes, kOnes, kOnes),
                             u256_le(kOnes, kOnes, kOnes, kOnes),
                             u256_le(0, 0, 0, 1)),
            (U256Results{u256_le(0, 0, 0, 1),
                         u256_le(kOnes, kOnes, kOnes, kOnes - 1),
                         u256_le(0, 0, 0, 0)}));
  // The largest product by the largest divisor.
  EXPECT_EQ(u256_mul_div_rem(u256_le(kOnes, kOnes, kOnes, kOnes),
                             u256_le(kOnes, kOnes, kOnes, kOnes),
                             u256_le(kOnes, kOnes, kOnes, kOnes)),
            (U256Results{u256_le(kOnes, kOnes, kOnes, kOnes),
                         u256_le(0, 0, 0, 0), u256_le(0, 0, 0, 0)}));
  // A one-limb divisor, 43, and the largest remainder it leaves.
  EXPECT_EQ(u256_mul_div_rem(u256_le(256, 0, 0, 12345),
                             u256_le(0, 3, kOnes, kOnes), u256_le(0, 0, 0, 43)),
            (U256Results{u256_le(0x5f41'7d05'f417'd059, 0x4d65'3594'd653'5dc9,
                                 0xc477'11dc'4771'1dc4, 0x7711'dc47'711d'c357),
                         u256_le(0, 0, 23, 0xd05f'417d'05f4'17d0),
                         u256_le(0, 0, 0, 42)}));
  // The division takes two off its estimate of nearly every digit.
  EXPECT_EQ(u256_mul_div_rem(u256_le(0x8000'0000'0000'0000,
                                     0x8000'0000'0000'0000,
                                     0x8000'0000'0000'0000, kOnes - 1),
                             u256_le(kOnes - 1, 1, 2, 0x0000'0000'ffff'ffff),
                             u256_le(0, 0, 2, 0x0000'0000'ffff'ffff)),
            (U256Results{u256_le(0xbeff'ffff'df7f'fffe, 0xefc0'0000'b7e0'0001,
                                 0xdbef'ffff'adf8'0000, 0xd6fb'ffff'eb7d'fffd),
                         u256_le(0, 0x3fff'ffff'dfff'ffff,
                                 0xefff'ffff'f7ff'ffff, 0xfbff'ffff'fe00'0000),
                         u256_le(0, 0, 1, 0xeb7e'0000'eb7d'ffff)}));
  // The division adds the divisor back for three digits of five.
  EXPECT_EQ(u256_mul_div_rem(u256_le(2, 1, kOnes, 2),
                             u256_le(kOnes - 1, kOnes - 1, kOnes,
                                     0x0000'0001'0000'0000),
                             u256_le(2, 1, kOnes, 3)),
            (U256Results{
                u256_le(kOnes - 1, kOnes - 1, kOnes - 1, 0x8000'0001'0000'0000),
                u256_le(0, 0, 0, 0),
                u256_le(2, 0, 0x8000'0000'0000'0002, 0x7fff'ffff'0000'0000)}));
  // The division meets equal top limbs for four digits of five.
  EXPECT_EQ(
      u256_mul_div_rem(u256_le(0xb1bb'6843'39f4'b9f1, 0xfc62'e952'8bb3'c8ac, 1,
                               0x2514'0294'fd85'93f7),
                       u256_le(kOnes - 1, kOnes, 0, 1),
                       u256_le(0xb1bb'6843'39f4'b9f1, 0xfc62'e952'8bb3'c8ac, 1,
                               0x2514'0294'fd85'93f8)),
      (U256Results{u256_le(kOnes - 1, kOnes - 1, kOnes, kOnes),
                   u256_le(0, 0, 0, 0),
                   u256_le(0x6376'd086'73e9'73e4, 0xf8c5'd2a5'1767'9159, 2,
                           0x4a28'0529'fb0b'27ef)}));
}

TEST_F(MathSyscallTestRv32Test, U128MulDivRemResults)
{
  // 6 * 7 / 4 is 10, remainder 2.
  EXPECT_EQ(u128_mul_div_rem(u128(0, 6), u128(0, 7), u128(0, 4)),
            (U128Results{u128(0, 10), u128(0, 0), u128(0, 2)}));
  // A quotient that needs both halves:
  // (2^127 + 2^64 + 5) * (2^65 + 1) = (2^128 + 2^65 + 2^63 + 11) * 2^64 + 5
  EXPECT_EQ(
      u128_mul_div_rem(u128(0x8000'0000'0000'0001, 5), u128(2, 1), u128(1, 0)),
      (U128Results{u128(2, 0x8000'0000'0000'000b), u128(0, 1), u128(0, 5)}));
  // The largest product, undivided: (2^128 - 1)^2 = (2^128 - 2) * 2^128 + 1.
  EXPECT_EQ(u128_mul_div_rem(u128(kOnes, kOnes), u128(kOnes, kOnes),
                             u128(0, 1)),
            (U128Results{u128(0, 1), u128(kOnes, kOnes - 1), u128(0, 0)}));
  // The largest product by the largest divisor.
  EXPECT_EQ(u128_mul_div_rem(u128(kOnes, kOnes), u128(kOnes, kOnes),
                             u128(kOnes, kOnes)),
            (U128Results{u128(kOnes, kOnes), u128(0, 0), u128(0, 0)}));
  // An amount times a rate over 10^18:
  // 123456789012345678901234567890 * 987654321098765432 / 10^18
  EXPECT_EQ(u128_mul_div_rem(u128(0x0000'0001'8ee9'0ff6, 0xc373'e0ee'4e3f'0ad2),
                             u128(0, 0x0db4'da5f'49f8'b478),
                             u128(0, 0x0de0'b6b3'a764'0000)),
            (U128Results{u128(0x0000'0001'89fc'4efa, 0xc819'3565'7ee9'5681),
                         u128(0, 0), u128(0, 0x01f1'a108'6d34'ba70)}));
  // The division takes two off its estimate of nearly every digit.
  EXPECT_EQ(u128_mul_div_rem(u128(kOnes, 0x7fff'ffff'ffff'ffff),
                             u128(kOnes - 1, 0x8000'0000'0000'0000),
                             u128(0x0000'0001'0000'0000, kOnes - 1)),
            (U128Results{u128(0xffff'ffff'0000'0002, 0xffff'fffa'c000'0003),
                         u128(0, 0x0000'0000'ffff'fffe),
                         u128(0x0000'0000'4000'0004, 0x7fff'fff5'8000'0006)}));
  // The division meets equal top limbs for two digits of three.
  EXPECT_EQ(u128_mul_div_rem(u128(kOnes, kOnes), u128(2, 0x7fff'ffff'ffff'ffff),
                             u128(2, 0x7fff'ffff'ffff'ffff)),
            (U128Results{u128(kOnes, kOnes), u128(0, 0), u128(0, 0)}));
}

// The guest checks each call against the SDK's software mul_div, which it
// stands in for, and against a * b = quotient * divisor + remainder.
TEST_F(MathSyscallTestRv32Test, U256MulDivRemMatchesSoftware)
{
  expect_method_passes(
      client_.make_invocation<u8"test_u256_mul_div_rem_matches_software">(
          kDefaultMaxTxGas),
      0);
}

TEST_F(MathSyscallTestRv32Test, U128MulDivRemMatchesSoftware)
{
  expect_method_passes(
      client_.make_invocation<u8"test_u128_mul_div_rem_matches_software">(
          kDefaultMaxTxGas),
      0);
}

// A zero divisor is an invalid input, not a fault, and writes nothing.
TEST_F(MathSyscallTestRv32Test, MulDivRemZeroDivisor)
{
  expect_method_passes(
      client_.make_invocation<u8"test_mul_div_rem_zero_divisor">(
          kDefaultMaxTxGas),
      0);
}

// For each of the two calls, 27 placements of an output on an input and 9 of
// an output on another output fault. Inputs that share a buffer, and outputs
// that only touch what is next to them, do not.
TEST_F(MathSyscallTestRv32Test, MulDivRemOverlap)
{
  expect_method_passes(client_.make_invocation<u8"test_mul_div_rem_overlap">(
                           kDefaultMaxTxGas),
                       72);
}

} // namespace hogd
