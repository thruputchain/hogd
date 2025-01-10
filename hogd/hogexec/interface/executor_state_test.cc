#include <gtest/gtest.h>

#include <barrier>

#include "hogd/base/worker_pool.h"
#include "hogd/hogexec/interface/executor_state.h"

namespace hogd {

TEST(ExecutorState, ProposalConditionVar)
{
  ExecutorState state;

  WorkerPool &pool = WorkerPool::get_default_pool();
  constexpr size_t kLeases = 10;

  state.start_proposing();

  for (auto i = 0U; i < kLeases; i++) {
    ASSERT_TRUE(state.try_acquire_run_lease(ExecutorMode::kProposing));
  }

  EXPECT_EQ(state.get_state(), ExecutorMode::kProposing);

  std::thread th([&state] { state.await_finish(); });

  std::vector<std::future<void>> futures(kLeases);

  // enqueue jobs
  for (auto &future : futures)
    future = pool.submit([&state] { state.release_run_lease(); });

  // wait for completions
  for (auto &future : futures)
    future.get();

  th.join();

  EXPECT_EQ(state.get_state(), ExecutorMode::kNone);
}

} // namespace hogd
