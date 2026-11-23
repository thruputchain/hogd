#include <atomic>
#include <chrono>
#include <csignal>
#include <semaphore>
#include <utility>

#include <gtest/gtest.h>

#include "x23/signal.h"

using namespace x23;
using namespace std::chrono;

namespace {

void
service_signals()
{
  std::binary_semaphore done(0);
  std::atomic<unsigned> passes{0};
  // Registration can happen during an existing service pass. Force another
  // full pass so earlier callbacks have run after this marker was registered.
  AutoSignalCbHandle marker = signal_cb(SIGUSR2, [&] {
    auto pass = passes.fetch_add(1);
    if (pass == 0)
      signal_handler(SIGUSR2);
    else if (pass == 1)
      done.release();
  });
  signal_handler(SIGUSR2);
  EXPECT_TRUE(done.try_acquire_for(seconds(5)));
}

} // namespace

TEST(SignalHandle, MoveConstructionAndReleaseTransferOwnership)
{
  std::atomic<unsigned> calls{0};
  AutoSignalCbHandle empty;
  AutoSignalCbHandle original = signal_cb(SIGUSR2, [&] { ++calls; });
  {
    AutoSignalCbHandle moved(std::move(original));
    empty = std::move(moved);
  }
  service_signals();
  EXPECT_GT(calls.load(), 0U);
  auto raw = empty.release();
  auto before = calls.load();
  service_signals();
  EXPECT_GT(calls.load(), before);
  {
    AutoSignalCbHandle reclaimed(std::move(raw));
  }
  before = calls.load();
  service_signals();
  EXPECT_EQ(calls.load(), before);
}

TEST(SignalHandle, MoveAssignmentCancelsOldCallbackAndHandlesSelfAssignment)
{
  std::atomic<unsigned> first_calls{0}, second_calls{0};
  AutoSignalCbHandle first = signal_cb(SIGUSR2, [&] { ++first_calls; });
  AutoSignalCbHandle second = signal_cb(SIGUSR2, [&] { ++second_calls; });
  service_signals();
  first = std::move(second);
  auto first_before = first_calls.load();
  auto &alias = first;
  first = std::move(alias);
  auto second_before = second_calls.load();
  service_signals();
  EXPECT_EQ(first_calls.load(), first_before);
  EXPECT_GT(second_calls.load(), second_before);
  first = AutoSignalCbHandle{};
  second_before = second_calls.load();
  service_signals();
  EXPECT_EQ(second_calls.load(), second_before);
}

TEST(SignalHandle, RawAssignmentReplacesCallbackAndReclaimsReleasedHandle)
{
  std::atomic<unsigned> first_calls{0}, second_calls{0};
  AutoSignalCbHandle handle = signal_cb(SIGUSR2, [&] { ++first_calls; });
  service_signals();
  handle = signal_cb(SIGUSR2, [&] { ++second_calls; });
  auto first_before = first_calls.load();
  auto second_before = second_calls.load();
  handle = handle.release();
  service_signals();
  EXPECT_EQ(first_calls.load(), first_before);
  EXPECT_GT(second_calls.load(), second_before);
}
