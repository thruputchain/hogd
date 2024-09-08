#include <gtest/gtest.h>

#include "hogd/base/types.h"
#include "hogd/syscall/metered_result.h"
#include "hogd/syscall/syscall_error_code.h"

#include <stdexcept>

namespace hogd {

TEST(MeteredResult, Chaining)
{
  MeteredResult<uint64_t> start = {5_gas, 100};

  auto res =
      start >> [](auto s) { return MeteredResult<uint64_t>(10_gas, s * 2); };

  EXPECT_TRUE(!!res);
  EXPECT_EQ(200, *res);
  EXPECT_EQ(15_gas, res.gas_consumed);
}

TEST(MeteredResult, CompilePtrTest)
{
  // check that -> expressions compile properly
  struct LocalCheck {
    uint64_t x;
  };

  auto res = MeteredResult<uint32_t>(10_gas, 100) >>
             [](auto val) { return MeteredResult<LocalCheck>(0_gas, val); };

  EXPECT_EQ((res->x), 100);
}

TEST(MeteredResult, ChainingErrors)
{

  auto res = MeteredResult<uint32_t>(10_gas, 100) >> [](auto) {
    return MeteredResult<void>(10_gas,
                               SyscallError{
                                   SyscallErrorCode::kUnrecoverableUnknown});
  } >> [] -> MeteredResult<void> { throw std::runtime_error("impossible"); };

  EXPECT_FALSE(!!res);
}

TEST(MeteredResult, GasCalculation)
{
  auto res = MeteredResult<uint32_t>(100_gas, 100) >> [](auto) {
    return MeteredResult<void>(100_gas,
                               SyscallError{
                                   SyscallErrorCode::kUnrecoverableUnknown});
  } >> [] -> MeteredResult<void> { return MeteredResult<void>(10000_gas); };

  EXPECT_FALSE(!!res);

  EXPECT_EQ(res.gas_consumed, 200_gas);
}

} // namespace hogd
