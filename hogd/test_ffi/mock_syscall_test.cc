#include <cstdint>
#include <gtest/gtest.h>

#include <cstddef>
#include <cstring>
#include <span>
#include <tuple>
#include <utility>
#include <vector>

#include "ffi_enums.hh"
#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h"
#include "hogd/hogexec/runtime/status.h"
#include "hogd/test_ffi/db_context_wrapper.h"
#include "hogd/test_ffi/mock_db_server.h"
#include "hogd/test_ffi/port_allocator.h"
#include "hogd/test_ffi/syscall_handler_wrapper.h"
#include "syscall.hh"
#include "transaction.hh"
#include "types.hh"
#include "x23/vec.h"
#include "x23/x23.h"

namespace hogd {

namespace {

// primitive type, int128, int128, opaque<>, optional
using input_types =
    std::tuple<uint32_t, int128_xdr, int128_xdr, x23::Vec<uint8_t, 100>,
               x23::Ptr<uint64_t>, x23::Ptr<uint32_t>>;
// primitive types, int128, opaque<>
using output_types =
    std::tuple<uint32_t, uint64_t, int128_xdr, x23::Vec<uint8_t, 50>>;

HostFnStatus<SyscallErrorCode>
mock_test_inner(uint32_t inp_1, int128_t const &inp_128,
                int128_t inp_128_by_value, std::span<const std::byte> inp_span,
                const uint64_t *inp_opt, const uint32_t *inp_opt_null,
                uint32_t &out_1, uint64_t &out_2, int128_t &out_128,
                std::span<std::byte> out_span)
{

  EXPECT_EQ(inp_1, 100);
  EXPECT_EQ(inp_128, int128_t{1} << 70);
  EXPECT_EQ(inp_128_by_value, int128_t{1} << 80);
  EXPECT_EQ(inp_span.size(), 6);
  if (inp_span.size() != 6) {
    return HostFnError{HostFnErrorCode::kUnrecoverable};
  }
  std::vector<uint8_t> expect_span = {0, 1, 2, 3, 4, 5};
  EXPECT_EQ(std::memcmp(inp_span.data(), expect_span.data(),
                        expect_span.size()),
            0);

  EXPECT_TRUE(inp_opt);
  if (!inp_opt) {
    return HostFnError{HostFnErrorCode::kUnrecoverable};
  }

  EXPECT_FALSE(inp_opt_null);

  out_1 = 10;
  out_2 = UINT64_MAX;
  out_128 = int128_t{out_1} + int128_t{out_2};

  EXPECT_EQ(out_span.size(), 10);
  out_span[5] = std::byte{10};
  out_span[7] = std::byte{200};

  // arbitrary nonzero return code
  return {SyscallErrorCode::kNoReturnBufferAvailable};
}

std::pair<HostFnStatus<SyscallErrorCode>, GasAmount>
int_type_tests(uint32_t inp_1, int128_t const &inp_128,
               int128_t inp_128_by_value, uint32_t &out_1, uint64_t &out_2,
               int128_t &out_128)
{
  EXPECT_EQ(inp_1, 100);
  EXPECT_EQ(inp_128, int128_t{1} << 70);
  EXPECT_EQ(inp_128_by_value, int128_t{1} << 80);

  out_1 = 10;
  out_2 = UINT64_MAX;
  out_128 = int128_t{out_1} + int128_t{out_2};
  return {{SyscallErrorCode::kNoReturnBufferAvailable}, 0};
}

} // namespace

TEST(MockSyscallTest, IntTypeTests)
{
  uint32_t port = PortAllocator::get();
  MockDBServer server(0);
  server.start_server(port);
  
  DBContext db_context(port);
  db_context.advance_new_block();

  SyscallHandlerContext context(nullptr, SignedTransaction{}, db_context);
  ASSERT_TRUE(context.push_context());

  uint32_t inp_1 = 100;
  int128_t inp_128 = int128_t{1} << 70;
  int128_t inp_128_by_value = int128_t{1} << 80;

  uint32_t out_1;
  uint64_t out_2;
  int128_t out_128;

  auto res = mock_syscall_parser<
      SyscallNo::kExit, std::tuple<uint32_t, int128_xdr, int128_xdr>,
      std::tuple<uint32_t, uint64_t, int128_xdr>, &int_type_tests>(
      inp_1, reinterpret_cast<uint64_t>(&inp_128),
      reinterpret_cast<uint64_t>(&inp_128_by_value),
      reinterpret_cast<uint64_t>(&out_1), reinterpret_cast<uint64_t>(&out_2),
      reinterpret_cast<uint64_t>(&out_128));
  context.pop_context();

  ASSERT_EQ(res.panic, 0);
  ASSERT_EQ(res.result_code,
            std::to_underlying(SyscallErrorCode::kNoReturnBufferAvailable));

  EXPECT_EQ(out_1, 10);
  EXPECT_EQ(out_2, UINT64_MAX);
  EXPECT_EQ(out_128, int128_t{10} + int128_t{UINT64_MAX});
}

} // namespace hogd
