#include <array>
#include <cstdint>
#include <gtest/gtest.h>

#include <cstddef>
#include <cstring>
#include <memory>
#include <span>
#include <string_view>
#include <utility>

#include "ffi_enums.hh"
#include "hogd/base/types.h"
#include "hogd/hogexec/precompiles/context.h"
#include "hogd/hogexec/runtime/status.h"
#include "hogd/hogexec/runtime/syscall_meta_concept.h"
#include "hogd/init.h"
#include "hogd/test_ffi/db_context_wrapper.h"
#include "hogd/test_ffi/mock_contract.h"
#include "hogd/test_ffi/mock_runtime.h"
#include "hogd/test_ffi/syscall_handler_wrapper.h"
#include "hogd/test_ffi/test_bindings.h"
#include "syscall.hh"
#include "syscall_error.hh"
#include "transaction.hh"
#include "types.hh"

namespace hogd {

namespace {

std::pair<HostFnStatus<SyscallErrorCode>, GasAmount>
int_type_test(uint32_t inp_1, int128_t const &inp_128,
              int128_t inp_128_by_value, uint32_t &out_1, uint64_t &out_2,
              int128_t &out_128)
{
  EXPECT_EQ(inp_1, 100);
  EXPECT_EQ(inp_128, int128_t{1} << 70);
  EXPECT_EQ(inp_128_by_value, int128_t{1} << 80);

  out_1 = 10;
  out_2 = UINT64_MAX;
  out_128 = int128_t{out_1} + int128_t{out_2};
  return {{SyscallErrorCode::kSyscallErrorCodeNoReturnBufferAvailable}, 0};
}

std::pair<HostFnStatus<SyscallErrorCode>, GasAmount>
span_type_test(std::span<const std::byte> span_in,
               std::span<std::byte> span_out)
{
  EXPECT_EQ(span_in.size(), 8);
  EXPECT_EQ(span_out.size(), 4);

  EXPECT_EQ(span_in[0], std::byte{10});
  EXPECT_EQ(span_in[1], std::byte{9});

  span_out[0] = std::byte{1};
  span_out[1] = std::byte{2};
  span_out[2] = std::byte{3};
  span_out[3] = std::byte{4};

  return {{SyscallErrorCode::kSyscallErrorCodeStorageKeyNotExist}, 0};
}

std::pair<HostFnStatus<SyscallErrorCode>, GasAmount>
no_return_test()
{
  return {{SyscallErrorCode::kSyscallErrorCodeNone}, 0};
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
};

struct SpanTypeTestSyscallMeta {
  constexpr static bool kIsReadOnlySyscall = true;
  constexpr static SyscallNo kSyscallNo = SyscallNo::kExit; // dummy syscall no
  constexpr static auto kInnerFunc = &span_type_test;
  constexpr static std::array<std::string_view, 1> kInputNames = {"span_in"};
  constexpr static std::array<std::string_view, 1> kOutputNames = {"span_out"};
  constexpr static bool kCanReturn = true;
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
};

TEST(MockSyscallTest, IntTypeTest)
{
  DBContext db_context;
  auto runtime_context =
      std::make_unique<hogd::MockRuntimeContext>(make_hogexec_context());

  SyscallHandlerContext context(runtime_context.get(), SignedTransaction{},
                                db_context);
  ASSERT_TRUE(context.push_context());

  MockRuntime mock_runtime(MockContract(dummy_entrypoint, nullptr));
  context.push_mock_runtime(&mock_runtime);
  mock_runtime.set_available_gas(1000);

  uint32_t inp_1 = 100;
  int128_t inp_128 = int128_t{1} << 70;
  int128_t inp_128_by_value = int128_t{1} << 80;

  uint32_t out_1;
  uint64_t out_2;
  int128_t out_128;

  auto res = mock_syscall_parser<IntTypeTestSyscallMeta>(
      inp_1, reinterpret_cast<uint64_t>(&inp_128),
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

TEST(MockSyscallTest, SpanTest)
{
  DBContext db_context;
  auto runtime_context =
      std::make_unique<hogd::MockRuntimeContext>(make_hogexec_context());
  SyscallHandlerContext context(runtime_context.get(), SignedTransaction{},
                                db_context);
  ASSERT_TRUE(context.push_context());

  MockRuntime mock_runtime(MockContract(dummy_entrypoint, nullptr));
  context.push_mock_runtime(&mock_runtime);
  mock_runtime.set_available_gas(1000);

  uint64_t inp = 0x090A;
  uint32_t out = 0;

  std::span<const std::byte> inp_span = std::as_bytes(std::span(&inp, 1));
  std::span<std::byte> out_span = std::as_writable_bytes(std::span(&out, 1));

  auto res = mock_syscall_parser<SpanTypeTestSyscallMeta>(
      reinterpret_cast<uint64_t>(inp_span.data()), inp_span.size(),
      reinterpret_cast<uint64_t>(out_span.data()), out_span.size(), 0, 0, 0);
  context.pop_mock_runtime();
  context.pop_context();

  ASSERT_EQ(res.panic, 0);
  ASSERT_EQ(res.result_code,
            std::to_underlying(
                SyscallErrorCode::kSyscallErrorCodeStorageKeyNotExist));

  EXPECT_EQ(out, 0x04030201);
}

TEST(MockSyscallTest, NoReturnTest)
{
  DBContext db_context;
  auto runtime_context =
      std::make_unique<hogd::MockRuntimeContext>(make_hogexec_context());
  SyscallHandlerContext context(runtime_context.get(), SignedTransaction{},
                                db_context);
  ASSERT_TRUE(context.push_context());

  MockRuntime mock_runtime(MockContract(dummy_entrypoint, nullptr));
  context.push_mock_runtime(&mock_runtime);
  mock_runtime.set_available_gas(1000);

  auto res = mock_syscall_parser<NoReturnSyscallMeta>(0, 0, 0, 0, 0, 0, 0);
  context.pop_mock_runtime();
  context.pop_context();

  ASSERT_EQ(res.panic, std::to_underlying(HostFnErrorCode::kHostExit));
  ASSERT_EQ(res.result_code, 0);
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
