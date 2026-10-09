#include <gtest/gtest.h>

#include <chrono>
#include <format>

#include "hogd/base/time.h"

namespace hogd {

TEST(TimeTest, UtcTimeFields)
{
  using namespace std::chrono;
  const system_clock::time_point tp = sys_days{year{2026} / 10 / 9} +
                                      hours{19} + minutes{4} + seconds{9} +
                                      nanoseconds{46318862};
  const UtcTime utc(tp);
  EXPECT_EQ(utc.year, 2026);
  EXPECT_EQ(utc.month, 10U);
  EXPECT_EQ(utc.day, 9U);
  EXPECT_EQ(utc.hours, 19);
  EXPECT_EQ(utc.minutes, 4);
  EXPECT_EQ(utc.seconds, 9);
}

TEST(TimeTest, UtcTimeIso8601MatchesChronoFormat)
{
  using namespace std::chrono;
  const system_clock::time_point tp =
      sys_days{year{2026} / 1 / 2} + hours{3} + minutes{4} + seconds{5} +
      duration_cast<system_clock::duration>(nanoseconds{6000});
  EXPECT_EQ(UtcTime(tp).iso8601(), std::format("{:%Y-%m-%dT%H:%M:%SZ}", tp));
}

TEST(TimeTest, UtcTimeBeforeEpoch)
{
  using namespace std::chrono;
  const UtcTime utc(sys_days{year{1969} / 12 / 31} + hours{23} + minutes{59} +
                    seconds{59});
  EXPECT_EQ(utc.year, 1969);
  EXPECT_EQ(utc.month, 12U);
  EXPECT_EQ(utc.day, 31U);
  EXPECT_EQ(utc.hours, 23);
  EXPECT_EQ(utc.minutes, 59);
  EXPECT_EQ(utc.seconds, 59);
  EXPECT_EQ(utc.subseconds, 0);
}

} // namespace hogd
