#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <string_view>
#include <utility>

#include "ffi_enums.hh"
#include "hogd/base/types.h"
#include "hogd/hogexec/common/status.h"
#include "hogd/hogexec/precompiles/context.h"
#include "hogd/hogexec/runtime/syscall_meta_concept.h"
#include "hogd/hogexec/runtime/syscall_types.h"
#include "hogd/init.h"
#include "hogd/storage_cost/storage_cost_model.h"
#include "hogd/test_ffi/db_context_wrapper.h"
#include "hogd/test_ffi/mock_contract.h"
#include "hogd/test_ffi/mock_runtime.h"
#include "hogd/test_ffi/syscall_handler_wrapper.h"
#include "hogd/test_ffi/test_bindings.h"
#include "syscall.hh"
#include "syscall_error.hh"
#include "transaction.hh"
#include "x23/cleanup.h"

namespace hogd {

namespace {

std::pair<HostFnStatus<SyscallErrorCode>, WrappedGasAmount>
int_type_test(uint32_t inp_1, int128_t const &inp_128,
              int128_t const &inp_128_by_value, uint32_t &out_1,
              uint64_t &out_2, int128_t &out_128) noexcept
{
  EXPECT_EQ(inp_1, 100);
  EXPECT_EQ(inp_128, int128_t{1} << 70);
  EXPECT_EQ(inp_128_by_value, int128_t{1} << 80);

  out_1 = 10;
  out_2 = UINT64_MAX;
  out_128 = int128_t{out_1} + int128_t{out_2};
  return {SyscallErrorCode::kSyscallErrorCodeNoReturnBufferAvailable,
          WrappedGasAmount{0}};
}

std::pair<HostFnStatus<SyscallErrorCode>, WrappedGasAmount>
span_type_test(std::span<const std::byte> span_in,
               std::span<std::byte> span_out) noexcept
{
  EXPECT_EQ(span_in.size(), 8);
  EXPECT_EQ(span_out.size(), 4);

  EXPECT_EQ(span_in[0], std::byte{10});
  EXPECT_EQ(span_in[1], std::byte{9});

  span_out[0] = std::byte{1};
  span_out[1] = std::byte{2};
  span_out[2] = std::byte{3};
  span_out[3] = std::byte{4};

  return {SyscallErrorCode::kSyscallErrorCodeStorageKeyNotExist,
          WrappedGasAmount{0}};
}

std::pair<HostFnStatus<SyscallErrorCode>, WrappedGasAmount>
no_return_test() noexcept
{
  return {SyscallErrorCode::kSyscallErrorCodeNone, WrappedGasAmount{0}};
}

std::pair<HostFnStatus<SyscallErrorCode>, WrappedGasAmount>
signed_i32_type_test(int32_t input_value) noexcept
{
  EXPECT_EQ(input_value, -1);
  return {SyscallErrorCode::kSyscallErrorCodeNone, WrappedGasAmount{0}};
}

std::pair<HostFnStatus<SyscallErrorCode>, WrappedGasAmount>
unsigned_u32_type_test(uint32_t input_value) noexcept
{
  // The inner syscall is only reached when the (by-value, non-pointer) slot
  // narrows cleanly to a u32; the test passes the largest valid u32.
  EXPECT_EQ(input_value, UINT32_MAX);
  return {SyscallErrorCode::kSyscallErrorCodeNone, WrappedGasAmount{0}};
}

std::pair<HostFnStatus<SyscallErrorCode>, WrappedGasAmount>
gas_metering_test() noexcept
{
  return {SyscallErrorCode::kSyscallErrorCodeNone, WrappedGasAmount{5}};
}

// just used to instantiate the mock contract
ForeignInvokeResult
dummy_entrypoint(void * /*unused*/, uint64_t /*unused*/)
{
  return ForeignInvokeResult{};
}

} // namespace

struct IntTypeTestSyscallMeta {
  // bypasses having to load the most recent call frame to check for the read
  // only flag
  constexpr static bool kIsReadOnlySyscall = true;
  constexpr static SyscallNo kSyscallNo = SyscallNo::kExit; // dummy syscall no
  constexpr static auto kInnerFunc = &int_type_test;
  constexpr static std::array<std::string_view, 3> kInputNames = {
      "inp_1", "inp_128", "inp_128_by_value"};
  constexpr static std::array<std::string_view, 3> kOutputNames = {"out_1",
                                                                   "out_2",
                                                                   "out_128"};
  constexpr static bool kCanReturn = true;
  constexpr static std::array<SyscallErrorCode, 1> kAllowedErrors = {
      SyscallErrorCode::kSyscallErrorCodeNoReturnBufferAvailable};
};

struct SpanTypeTestSyscallMeta {
  constexpr static bool kIsReadOnlySyscall = true;
  constexpr static SyscallNo kSyscallNo = SyscallNo::kExit; // dummy syscall no
  constexpr static auto kInnerFunc = &span_type_test;
  constexpr static std::array<std::string_view, 1> kInputNames = {"span_in"};
  constexpr static std::array<std::string_view, 1> kOutputNames = {"span_out"};
  constexpr static bool kCanReturn = true;
  constexpr static std::array<SyscallErrorCode, 1> kAllowedErrors = {
      SyscallErrorCode::kSyscallErrorCodeStorageKeyNotExist};
};
static_assert(is_syscall_meta<IntTypeTestSyscallMeta>);
static_assert(is_syscall_meta<SpanTypeTestSyscallMeta>);

struct NoReturnSyscallMeta {
  constexpr static bool kIsReadOnlySyscall = true;
  constexpr static SyscallNo kSyscallNo =
      SyscallNo::kGetReturnedBuffer; // dummy syscall no
  constexpr static auto kInnerFunc = &no_return_test;
  constexpr static std::array<std::string_view, 0> kInputNames = {};
  constexpr static std::array<std::string_view, 0> kOutputNames = {};
  constexpr static bool kCanReturn = false;
  constexpr static std::array<SyscallErrorCode, 0> kAllowedErrors = {};
};

struct SignedI32TypeTestSyscallMeta {
  constexpr static bool kIsReadOnlySyscall = true;
  constexpr static SyscallNo kSyscallNo = SyscallNo::kExit; // dummy syscall no
  constexpr static auto kInnerFunc = &signed_i32_type_test;
  constexpr static std::array<std::string_view, 1> kInputNames = {
      "input_value"};
  constexpr static std::array<std::string_view, 0> kOutputNames = {};
  constexpr static bool kCanReturn = true;
  constexpr static std::array<SyscallErrorCode, 0> kAllowedErrors = {};
};

struct UnsignedU32TypeTestSyscallMeta {
  constexpr static bool kIsReadOnlySyscall = true;
  constexpr static SyscallNo kSyscallNo = SyscallNo::kExit; // dummy syscall no
  constexpr static auto kInnerFunc = &unsigned_u32_type_test;
  constexpr static std::array<std::string_view, 1> kInputNames = {
      "input_value"};
  constexpr static std::array<std::string_view, 0> kOutputNames = {};
  constexpr static bool kCanReturn = true;
  constexpr static std::array<SyscallErrorCode, 0> kAllowedErrors = {};
};

struct GasMeteringTestSyscallMeta {
  constexpr static bool kIsReadOnlySyscall = true;
  constexpr static SyscallNo kSyscallNo = SyscallNo::kExit; // dummy syscall no
  constexpr static auto kInnerFunc = &gas_metering_test;
  constexpr static std::array<std::string_view, 0> kInputNames = {};
  constexpr static std::array<std::string_view, 0> kOutputNames = {};
  constexpr static bool kCanReturn = true;
  constexpr static std::array<SyscallErrorCode, 0> kAllowedErrors = {};
};

TEST(MockSyscallTest, IntTypeTest)
{
  DBContext db_context(StorageCostModel::default_cost_model().to_xdr());
  auto runtime_context =
      std::make_unique<hogd::MockRuntimeContext>(make_hogexec_context());

  SyscallHandlerContext context(runtime_context.get(), SignedTransaction{},
                                db_context);
  ASSERT_TRUE(context.push_context());

  MockRuntime mock_runtime(MockContract(dummy_entrypoint, nullptr));
  context.push_mock_runtime(&mock_runtime);

  uint32_t inp_1 = 100;
  int128_t inp_128 = int128_t{1} << 70;
  int128_t inp_128_by_value = int128_t{1} << 80;

  uint32_t out_1;
  uint64_t out_2;
  int128_t out_128;

  auto res = mock_syscall_parser<IntTypeTestSyscallMeta>(
      SyscallNo::kExit, inp_1, reinterpret_cast<uint64_t>(&inp_128),
      reinterpret_cast<uint64_t>(&inp_128_by_value),
      reinterpret_cast<uint64_t>(&out_1), reinterpret_cast<uint64_t>(&out_2),
      reinterpret_cast<uint64_t>(&out_128), 0);
  context.pop_mock_runtime();
  context.pop_context();

  ASSERT_EQ(res.panic, 0);
  ASSERT_EQ(res.result_code,
            std::to_underlying(
                SyscallErrorCode::kSyscallErrorCodeNoReturnBufferAvailable));

  EXPECT_EQ(out_1, 10);
  EXPECT_EQ(out_2, UINT64_MAX);
  EXPECT_EQ(out_128, int128_t{10} + int128_t{UINT64_MAX});
}

TEST(MockSyscallTest, MockRuntimeConsumesReturnedGas)
{
  DBContext db_context(StorageCostModel::default_cost_model().to_xdr());
  auto runtime_context =
      std::make_unique<hogd::MockRuntimeContext>(make_hogexec_context());
  SyscallHandlerContext context(runtime_context.get(), SignedTransaction{},
                                db_context);
  ASSERT_TRUE(context.push_context());

  EmptyRuntime runtime(Invocation{.gas_limit = 7});
  context.push_mock_runtime(&runtime);

  auto res =
      mock_syscall_parser<GasMeteringTestSyscallMeta>(SyscallNo::kExit, 0, 0, 0,
                                                      0, 0, 0, 0);
  ASSERT_EQ(res.panic, 0);
  ASSERT_EQ(res.result_code,
            std::to_underlying(SyscallErrorCode::kSyscallErrorCodeNone));
  EXPECT_EQ(runtime.gas_consumed(), 5);
  EXPECT_EQ(runtime.gas_remaining(), 2);

  auto out_of_gas =
      mock_syscall_parser<GasMeteringTestSyscallMeta>(SyscallNo::kExit, 0, 0, 0,
                                                      0, 0, 0, 0);
  EXPECT_EQ(out_of_gas.panic, std::to_underlying(HostFnErrorCode::OutOfGas));
  EXPECT_EQ(out_of_gas.result_code, 0);
  EXPECT_EQ(runtime.gas_consumed(), 7);
  EXPECT_EQ(runtime.gas_remaining(), 0);

  context.pop_mock_runtime();
  context.pop_context();
}

TEST(MockSyscallTest, SpanTest)
{
  DBContext db_context(StorageCostModel::default_cost_model().to_xdr());
  auto runtime_context =
      std::make_unique<hogd::MockRuntimeContext>(make_hogexec_context());
  SyscallHandlerContext context(runtime_context.get(), SignedTransaction{},
                                db_context);
  ASSERT_TRUE(context.push_context());

  MockRuntime mock_runtime(MockContract(dummy_entrypoint, nullptr));
  context.push_mock_runtime(&mock_runtime);

  uint64_t inp = 0x090A;
  uint32_t out = 0;

  std::span<const std::byte> inp_span = std::as_bytes(std::span(&inp, 1));
  std::span<std::byte> out_span = std::as_writable_bytes(std::span(&out, 1));

  auto res = mock_syscall_parser<SpanTypeTestSyscallMeta>(
      SyscallNo::kExit, reinterpret_cast<uint64_t>(inp_span.data()),
      inp_span.size(), reinterpret_cast<uint64_t>(out_span.data()),
      out_span.size(), 0, 0, 0);
  context.pop_mock_runtime();
  context.pop_context();

  ASSERT_EQ(res.panic, 0);
  ASSERT_EQ(res.result_code,
            std::to_underlying(
                SyscallErrorCode::kSyscallErrorCodeStorageKeyNotExist));

  EXPECT_EQ(out, 0x04030201);
}

TEST(MockSyscallTest, ParseMockSignedNarrowCanonicality)
{
  DBContext db_context(StorageCostModel::default_cost_model().to_xdr());
  auto runtime_context =
      std::make_unique<hogd::MockRuntimeContext>(make_hogexec_context());
  SyscallHandlerContext context(runtime_context.get(), SignedTransaction{},
                                db_context);
  ASSERT_TRUE(context.push_context());

  MockRuntime mock_runtime(MockContract(dummy_entrypoint, nullptr));
  context.push_mock_runtime(&mock_runtime);

  // Under the wasm32 ABI each value slot carries a bare 32-bit value. The
  // canonical encoding of int32_t -1 is therefore 0xFFFFFFFF in the low 32
  // bits, with the upper 32 bits zero. (signed_i32_type_test asserts the
  // parsed value is -1.)
  auto res_canonical =
      mock_syscall_parser<SignedI32TypeTestSyscallMeta>(SyscallNo::kExit,
                                                        0x00000000FFFFFFFFULL,
                                                        0, 0, 0, 0, 0, 0);
  ASSERT_EQ(res_canonical.panic, 0);
  ASSERT_EQ(res_canonical.result_code,
            std::to_underlying(SyscallErrorCode::kSyscallErrorCodeNone));

  // The mock keeps u64 slots so it can carry host pointers, but a value slot
  // must still fit the real 32-bit ABI slot. A value with any bits set above
  // bit 31 cannot have come from the runtime, so the mock faults it (returns
  // an FFI error) rather than silently truncating. UINT64_MAX -- the old
  // sign-extended-to-64 encoding of -1 -- is now rejected.
  auto res_high_bits =
      mock_syscall_parser<SignedI32TypeTestSyscallMeta>(SyscallNo::kExit,
                                                        UINT64_MAX, 0, 0, 0, 0,
                                                        0, 0);
  ASSERT_EQ(res_high_bits.panic, 0);
  ASSERT_EQ(res_high_bits.result_code,
            std::to_underlying(SyscallErrorCode::kSyscallErrorCodeFault));

  // One bit above UINT32_MAX also faults (low 32 bits zero, but it still does
  // not fit a 32-bit slot).
  auto res_overflow =
      mock_syscall_parser<SignedI32TypeTestSyscallMeta>(SyscallNo::kExit,
                                                        0x0000000100000000ULL,
                                                        0, 0, 0, 0, 0, 0);
  ASSERT_EQ(res_overflow.panic, 0);
  ASSERT_EQ(res_overflow.result_code,
            std::to_underlying(SyscallErrorCode::kSyscallErrorCodeFault));

  context.pop_mock_runtime();
  context.pop_context();
}

// A value-typed (non-pointer/offset) slot that does not narrow down to a u32
// is rejected with a fault, rather than silently truncated. The mock keeps u64
// slots so it can carry host pointers in offset slots, but value slots must
// still fit the real 32-bit wasm32 ABI slot.
TEST(MockSyscallTest, ParseMockUnsignedSlotOverflow)
{
  DBContext db_context(StorageCostModel::default_cost_model().to_xdr());
  auto runtime_context =
      std::make_unique<hogd::MockRuntimeContext>(make_hogexec_context());
  SyscallHandlerContext context(runtime_context.get(), SignedTransaction{},
                                db_context);
  ASSERT_TRUE(context.push_context());

  MockRuntime mock_runtime(MockContract(dummy_entrypoint, nullptr));
  context.push_mock_runtime(&mock_runtime);

  // The largest valid u32 narrows cleanly and reaches the inner syscall.
  auto res_max =
      mock_syscall_parser<UnsignedU32TypeTestSyscallMeta>(SyscallNo::kExit,
                                                          0x00000000FFFFFFFFULL,
                                                          0, 0, 0, 0, 0, 0);
  ASSERT_EQ(res_max.panic, 0);
  ASSERT_EQ(res_max.result_code,
            std::to_underlying(SyscallErrorCode::kSyscallErrorCodeNone));

  // Exactly one past UINT32_MAX does not fit a 32-bit slot -> fault.
  auto res_overflow =
      mock_syscall_parser<UnsignedU32TypeTestSyscallMeta>(SyscallNo::kExit,
                                                          0x0000000100000000ULL,
                                                          0, 0, 0, 0, 0, 0);
  ASSERT_EQ(res_overflow.panic, 0);
  ASSERT_EQ(res_overflow.result_code,
            std::to_underlying(SyscallErrorCode::kSyscallErrorCodeFault));

  // A fully out-of-range value also faults.
  auto res_max_u64 =
      mock_syscall_parser<UnsignedU32TypeTestSyscallMeta>(SyscallNo::kExit,
                                                          UINT64_MAX, 0, 0, 0, 0,
                                                          0, 0);
  ASSERT_EQ(res_max_u64.panic, 0);
  ASSERT_EQ(res_max_u64.result_code,
            std::to_underlying(SyscallErrorCode::kSyscallErrorCodeFault));

  context.pop_mock_runtime();
  context.pop_context();
}

TEST(MockSyscallTest, NoReturnTest)
{
  DBContext db_context(StorageCostModel::default_cost_model().to_xdr());
  auto runtime_context =
      std::make_unique<hogd::MockRuntimeContext>(make_hogexec_context());
  SyscallHandlerContext context(runtime_context.get(), SignedTransaction{},
                                db_context);
  ASSERT_TRUE(context.push_context());

  MockRuntime mock_runtime(MockContract(dummy_entrypoint, nullptr));
  context.push_mock_runtime(&mock_runtime);

  auto res =
      mock_syscall_parser<NoReturnSyscallMeta>(SyscallNo::kGetReturnedBuffer, 0,
                                               0, 0, 0, 0, 0, 0);
  context.pop_mock_runtime();
  context.pop_context();

  ASSERT_EQ(res.panic, std::to_underlying(HostFnErrorCode::Exit));
  ASSERT_EQ(res.result_code, 0);
}

TEST(MockSyscallTest, NoSyscallTest)
{
  auto db_result = create_db(nullptr, 0);
  ASSERT_EQ(db_result.ffi_error_code, kTestFFIErrorCodeNone);
  x23::Defer free_db_defer([&] { free_db(db_result.db); });
  auto &db_context = *static_cast<DBContext *>(db_result.db);

  auto runtime_context =
      std::make_unique<hogd::MockRuntimeContext>(make_hogexec_context());
  SyscallHandlerContext context(runtime_context.get(), SignedTransaction{},
                                db_context);
  ASSERT_TRUE(context.push_context());

  MockRuntime mock_runtime(MockContract(dummy_entrypoint, nullptr));
  context.push_mock_runtime(&mock_runtime);

  auto res = mock_syscall_entrypoint(-1, 0, 0, 0, 0, 0, 0, 0);

  context.pop_mock_runtime();
  context.pop_context();

  ASSERT_EQ(res.panic, 0);
  ASSERT_EQ(res.result_code,
            std::to_underlying(SyscallErrorCode::kSyscallErrorCodeNoSyscall));
}

} // namespace hogd

int
main(int argc, char *argv[])
{
  auto ret = hogd::hogd_init(argc, argv, false);
  if (ret != 0)
    return ret;
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
