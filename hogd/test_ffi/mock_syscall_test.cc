#include <cstdint>
#include <gtest/gtest.h>

#include <cstddef>
#include <cstring>
#include <span>
#include <tuple>
#include <utility>

#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h"
#include "hogd/hogexec/runtime/status.h"
#include "hogd/hogexec/runtime/test_util/block_snapshot_server.h"
#include "hogd/test_ffi/db_context_wrapper.h"
#include "hogd/test_ffi/port_allocator.h"
#include "hogd/test_ffi/syscall_handler_wrapper.h"
#include "syscall.hh"
#include "transaction.hh"
#include "types.hh"
#include "x23/vec.h"

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
  return {{SyscallErrorCode::kNoReturnBufferAvailable}, 0};
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

  return {{SyscallErrorCode::kStorageKeyNotExist}, 0};
}

} // namespace

TEST(MockSyscallTest, IntTypeTest)
{
  uint32_t port = PortAllocator::get();
  BlockSnapshotServer server(0);
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
      std::tuple<uint32_t, uint64_t, int128_xdr>, &int_type_test>(
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

TEST(MockSyscallTest, SpanTest)
{
  uint32_t port = PortAllocator::get();
  BlockSnapshotServer server(0);
  server.start_server(port);

  DBContext db_context(port);
  db_context.advance_new_block();

  SyscallHandlerContext context(nullptr, SignedTransaction{}, db_context);
  ASSERT_TRUE(context.push_context());

  uint64_t inp = 0x090A;
  uint32_t out = 0;

  std::span<const std::byte> inp_span = std::as_bytes(std::span(&inp, 1));
  std::span<std::byte> out_span = std::as_writable_bytes(std::span(&out, 1));

  auto res =
      mock_syscall_parser<SyscallNo::kExit, std::tuple<x23::Vec<uint8_t>>,
                          std::tuple<x23::Vec<uint8_t>>, &span_type_test>(
          reinterpret_cast<uint64_t>(inp_span.data()), inp_span.size(),
          reinterpret_cast<uint64_t>(out_span.data()), out_span.size(), 0, 0);
  context.pop_context();

  ASSERT_EQ(res.panic, 0);
  ASSERT_EQ(res.result_code,
            std::to_underlying(SyscallErrorCode::kStorageKeyNotExist));

  EXPECT_EQ(out, 0x04030201);
}

} // namespace hogd
