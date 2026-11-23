// average_age.h - the token-weighted average age of two groups of native
// tokens, shared by the mempool scheduler and hog_db's startup reconciliation

#pragma once

#include <utility>

#include "hogd/base/compiler.h"
#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "hogd/base/wide_math.h"
#include "types.hh"

namespace hogd {

namespace average_age_detail {

// Returns ceil(n * d / t) for 0 <= d, 0 < t and n <= t, which is at most d.
// Falls back to wide_math only in the rare case that n * d overflows uint128_t.
inline BlockHeight
mul_div_round_up(uint128_t n, BlockHeight d, uint128_t t)
{
  const auto wide_d = static_cast<uint128_t>(d);
  const auto [product, overflow] = overflowing_mul(n, wide_d);
  if (!overflow) [[likely]] {
    return static_cast<BlockHeight>((product / t) + (product % t != 0 ? 1 : 0));
  }
  const auto divided =
      limbs_mul_div_rem<2>(limbs_from_u128(n), limbs_from_u128(wide_d),
                           limbs_from_u128(t));
  // Not empty: t is not zero. operator* rather than value(), which can throw.
  CHECK(divided.has_value());
  // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
  const LimbsMulDivRem<2> &result = *divided;
  return static_cast<BlockHeight>(result.quotient_lo[0]) +
         (result.remainder != Limbs<2>{} ? 1 : 0);
}

} // namespace average_age_detail

// The average age of [num_native_tokens_a] tokens at [age_a] and
// [num_native_tokens_b] tokens at [age_b], rounded up. The counts must have a
// nonzero sum that fits in uint128_t.
inline BlockHeight
compute_average_age(uint128_t num_native_tokens_a, BlockHeight age_a,
                    uint128_t num_native_tokens_b, BlockHeight age_b)
{
  uint128_t denominator = num_native_tokens_a + num_native_tokens_b;
  CHECK_GT(denominator, 0UL);
  if (age_a > age_b) {
    std::swap(num_native_tokens_a, num_native_tokens_b);
    std::swap(age_a, age_b);
  }
  // age_a plus b's weighted share of the gap: stays between the two ages and
  // avoids summing token * age products, which can overflow uint128_t.
  //
  // When doing division, we introduce imprecision into our calculations. Thus,
  // we always round the result up to avoid benefitting the account. This helps
  // to ensure that users are not incentivized to waste network storage or
  // compute, such as by spreading native tokens among multiple accounts.
  return age_a + average_age_detail::mul_div_round_up(num_native_tokens_b,
                                                      age_b - age_a,
                                                      denominator);
}

} // namespace hogd
