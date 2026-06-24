#include <atomic>
#include <chrono>
#include <stdexcept>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "x23/dag.h"
#include "x23/event.h"
#include "x23/threadpool.h"

using namespace std::chrono_literals;
using namespace x23;

ThreadPool pool(4);
CoroPoll cp(pool);

static void
sleep_for_cpu_accounting_test()
{
  std::this_thread::sleep_for(40ms);
}

TEST(Dag, RunsDependenciesAndStoresTypedResults)
{
  Dag dag("basic", pool);

  auto a = dag.cpu<int>("a", [] { return 7; });
  auto b = dag.async<int>("b", dag.after(a),
                          [&]() -> Async<int> { co_return a.get() + 5; });
  auto c =
      dag.cpu<int>("c", dag.after(a, b), [&] { return a.get() * b.get(); });

  dag.run()();

  EXPECT_TRUE(dag.ok());
  EXPECT_TRUE(a.ready());
  EXPECT_TRUE(b.ready());
  EXPECT_TRUE(c.ready());
  EXPECT_EQ(a.get(), 7);
  EXPECT_EQ(b.get(), 12);
  EXPECT_EQ(c.get(), 84);
}

TEST(Dag, VoidNodesCanBeDependencies)
{
  Dag dag("void", pool);
  std::atomic_int value{0};

  auto set =
      dag.cpu<void>("set", [&] { value.store(10, std::memory_order_release); });
  auto add = dag.cpu<int>("add", dag.after(set), [&] {
    return value.load(std::memory_order_acquire) + 1;
  });

  dag.run()();

  EXPECT_TRUE(dag.ok());
  EXPECT_TRUE(set.ready());
  EXPECT_EQ(add.get(), 11);
}

TEST(Dag, IndependentRootsOverlapBeforeDependentStarts)
{
  Dag dag("overlap", pool);
  std::atomic_bool a_started{false};
  std::atomic_bool b_started{false};
  std::atomic_bool dependent_started{false};
  CoroCB<int> finish_a;
  CoroCB<int> finish_b;

  auto a = dag.async<int>("a", [&]() -> Async<int> {
    auto pending_pair = make_async<int>();
    Async<int> pending = std::move(pending_pair.first);
    finish_a = pending_pair.second;
    a_started.store(true, std::memory_order_release);
    a_started.notify_all();
    co_return co_await pending;
  });
  auto b = dag.async<int>("b", [&]() -> Async<int> {
    auto pending_pair = make_async<int>();
    Async<int> pending = std::move(pending_pair.first);
    finish_b = pending_pair.second;
    b_started.store(true, std::memory_order_release);
    b_started.notify_all();
    co_return co_await pending;
  });
  auto c = dag.cpu<int>("c", dag.after(a, b), [&] {
    dependent_started.store(true, std::memory_order_release);
    return a.get() + b.get();
  });

  std::atomic_bool done{false};
  [](Dag &dag, std::atomic_bool &done) -> Detached {
    co_await dag.run();
    done.store(true, std::memory_order_release);
    done.notify_all();
  }(dag, done);

  const auto deadline = std::chrono::steady_clock::now() + 1s;
  while (std::chrono::steady_clock::now() < deadline &&
         (!a_started.load(std::memory_order_acquire) ||
          !b_started.load(std::memory_order_acquire))) {
    std::this_thread::sleep_for(1ms);
  }
  EXPECT_TRUE(a_started.load(std::memory_order_acquire));
  EXPECT_TRUE(b_started.load(std::memory_order_acquire));
  EXPECT_FALSE(dependent_started.load(std::memory_order_acquire));

  ASSERT_TRUE(finish_a);
  ASSERT_TRUE(finish_b);
  finish_a(1);
  finish_b(2);
  const auto done_deadline = std::chrono::steady_clock::now() + 1s;
  while (std::chrono::steady_clock::now() < done_deadline &&
         !done.load(std::memory_order_acquire)) {
    std::this_thread::sleep_for(1ms);
  }

  EXPECT_TRUE(done.load(std::memory_order_acquire));
  EXPECT_TRUE(dag.ok());
  EXPECT_EQ(c.get(), 3);
}

TEST(Dag, AsyncNodeCanBeCompletedLater)
{
  Dag dag("async", pool);
  CoroCB<int> finish;
  std::atomic_bool async_started{false};

  auto a = dag.async<int>("a", [&]() -> Async<int> {
    auto pending_pair = make_async<int>();
    Async<int> pending = std::move(pending_pair.first);
    finish = pending_pair.second;
    async_started.store(true, std::memory_order_release);
    async_started.notify_all();
    co_return co_await pending;
  });
  auto b = dag.cpu<int>("b", dag.after(a), [&] { return a.get() * 2; });

  std::atomic_bool done{false};
  [](Dag &dag, std::atomic_bool &done) -> Detached {
    co_await dag.run();
    done.store(true, std::memory_order_release);
    done.notify_all();
  }(dag, done);

  while (!async_started.load(std::memory_order_acquire))
    async_started.wait(false);
  ASSERT_TRUE(finish);
  EXPECT_FALSE(done.load(std::memory_order_acquire));

  finish(21);
  while (!done.load(std::memory_order_acquire))
    done.wait(false);

  EXPECT_TRUE(dag.ok());
  EXPECT_EQ(b.get(), 42);
}

TEST(Dag, ExceptionFailsGraphAndSuppressesDownstreamNodes)
{
  Dag dag("exception", pool);
  std::atomic_bool downstream_ran{false};

  auto a = dag.cpu<int>("a", []() -> int { throw std::runtime_error("boom"); });
  dag.cpu<int>("downstream", dag.after(a), [&] {
    downstream_ran.store(true, std::memory_order_relaxed);
    return 1;
  });

  dag.run()();

  EXPECT_FALSE(dag.ok());
  EXPECT_EQ(dag.failure().kind, DagFailureKind::Exception);
  EXPECT_EQ(dag.failure().node, "a");
  EXPECT_FALSE(a.ready());
  EXPECT_FALSE(downstream_ran.load(std::memory_order_relaxed));
}

TEST(Dag, ExplicitFailureSuppressesDownstreamNodes)
{
  Dag dag("explicit-failure", pool);
  std::atomic_bool downstream_ran{false};

  auto a = dag.async<void>("a", [&]() -> Async<void> {
    dag.fail(DagFailureKind::Cancelled, "a");
    co_return;
  });
  dag.cpu<void>("downstream", dag.after(a),
                [&] { downstream_ran.store(true, std::memory_order_relaxed); });

  dag.run()();

  EXPECT_FALSE(dag.ok());
  EXPECT_EQ(dag.failure().kind, DagFailureKind::Cancelled);
  EXPECT_EQ(dag.failure().node, "a");
  EXPECT_TRUE(a.ready());
  EXPECT_FALSE(downstream_ran.load(std::memory_order_relaxed));
}

TEST(Dag, RunningSiblingsDrainAfterFailure)
{
  Dag dag("drain", pool);
  std::atomic_bool slow_started{false};
  std::atomic_bool release_slow{false};
  std::atomic_bool slow_finished{false};

  dag.cpu<void>("fail", [&] { throw std::runtime_error("boom"); });
  dag.cpu<void>("slow", [&] {
    slow_started.store(true, std::memory_order_release);
    slow_started.notify_all();
    while (!release_slow.load(std::memory_order_acquire))
      release_slow.wait(false);
    slow_finished.store(true, std::memory_order_release);
  });

  std::atomic_bool done{false};
  [](Dag &dag, std::atomic_bool &done) -> Detached {
    co_await dag.run();
    done.store(true, std::memory_order_release);
    done.notify_all();
  }(dag, done);

  while (!slow_started.load(std::memory_order_acquire))
    slow_started.wait(false);

  std::this_thread::sleep_for(20ms);
  EXPECT_FALSE(done.load(std::memory_order_acquire));
  release_slow.store(true, std::memory_order_release);
  release_slow.notify_all();

  while (!done.load(std::memory_order_acquire))
    done.wait(false);
  EXPECT_FALSE(dag.ok());
  EXPECT_TRUE(slow_finished.load(std::memory_order_acquire));
}

TEST(Dag, StatsRecordStartedAndFinishedNodes)
{
  Dag dag("stats", pool);

  auto a = dag.cpu<int>("a", [] { return 1; });
  dag.cpu<int>("b", dag.after(a), [&] { return a.get() + 1; });

  dag.run()();

  const std::vector<DagNodeStats> stats = dag.stats();
  ASSERT_EQ(stats.size(), 2UL);
  EXPECT_EQ(stats[0].name, "a");
  EXPECT_EQ(stats[1].name, "b");
  EXPECT_TRUE(stats[0].started);
  EXPECT_TRUE(stats[0].finished);
  EXPECT_TRUE(stats[1].started);
  EXPECT_TRUE(stats[1].finished);
  EXPECT_LE(stats[0].started_at, stats[0].finished_at);
  EXPECT_LE(stats[1].started_at, stats[1].finished_at);
  EXPECT_GE(stats[0].cpu_time.count(), 0);
  EXPECT_GE(stats[1].cpu_time.count(), 0);
}

TEST(Dag, CpuTimeExcludesCpuNodeSleep)
{
  Dag dag("cpu sleep", pool);

  dag.cpu<void>("sleep", [] { sleep_for_cpu_accounting_test(); });

  dag.run()();

  const std::vector<DagNodeStats> stats = dag.stats();
  ASSERT_EQ(stats.size(), 1UL);
  const auto wall_time = stats[0].finished_at - stats[0].started_at;
  EXPECT_GE(wall_time, 20ms);
  EXPECT_LT(stats[0].cpu_time, wall_time / 2);
}

TEST(Dag, CpuTimeExcludesAsyncSuspension)
{
  Dag dag("async wait", pool);
  CoroCB<void> finish;
  std::atomic_bool async_started{false};

  dag.async<void>("wait", [&]() -> Async<void> {
    auto pending_pair = make_async<void>();
    Async<void> pending = std::move(pending_pair.first);
    finish = pending_pair.second;
    async_started.store(true, std::memory_order_release);
    async_started.notify_all();
    co_await pending;
  });

  std::atomic_bool done{false};
  [](Dag &dag, std::atomic_bool &done) -> Detached {
    co_await dag.run();
    done.store(true, std::memory_order_release);
    done.notify_all();
  }(dag, done);

  while (!async_started.load(std::memory_order_acquire))
    async_started.wait(false);
  std::this_thread::sleep_for(40ms);
  ASSERT_TRUE(finish);
  finish();

  while (!done.load(std::memory_order_acquire))
    done.wait(false);

  const std::vector<DagNodeStats> stats = dag.stats();
  ASSERT_EQ(stats.size(), 1UL);
  const auto wall_time = stats[0].finished_at - stats[0].started_at;
  EXPECT_GE(wall_time, 20ms);
  EXPECT_LT(stats[0].cpu_time, wall_time / 2);
}

TEST(Dag, DotIncludesNodesAndEdges)
{
  Dag dag("test dag", pool);

  auto root = dag.cpu<int>("root", [] { return 1; });
  dag.cpu<void>("quote \"slash\\\"", dag.after(root), [] {});

  const std::string dot = dag.dot();
  EXPECT_NE(dot.find("digraph test_dag"), std::string::npos);
  EXPECT_NE(dot.find("dag_start [label=\"start\"]"), std::string::npos);
  EXPECT_NE(dot.find("dag_end [label=\"end\"]"), std::string::npos);
  EXPECT_NE(dot.find("n0 [label=\"0: root\"]"), std::string::npos);
  EXPECT_NE(dot.find("n1 [label=\"1: quote \\\"slash\\\\\\\"\"]"),
            std::string::npos);
  EXPECT_NE(dot.find("dag_start -> n0"), std::string::npos);
  EXPECT_NE(dot.find("n0 -> n1"), std::string::npos);
  EXPECT_NE(dot.find("n1 -> dag_end"), std::string::npos);
  EXPECT_EQ(dot.find("-> dag_start"), std::string::npos);
  EXPECT_EQ(dot.find("dag_end ->"), std::string::npos);
}

TEST(Dag, DotIncludesRuntimeStatsAfterRun)
{
  Dag dag("runtime stats", pool);

  auto root = dag.cpu<int>("root", [] { return 1; });
  dag.cpu<void>("child", dag.after(root), [] {});

  dag.run()();

  const std::string dot = dag.dot();
  EXPECT_NE(dot.find("dag_end [label=\"end\\ntime: "), std::string::npos);
  EXPECT_NE(dot.find("n0 [label=\"0: root\\nstate: done\\nwait: "),
            std::string::npos);
  EXPECT_NE(dot.find("\\nrun: "), std::string::npos);
  EXPECT_NE(dot.find("\\ntotal: "), std::string::npos);
  EXPECT_NE(dot.find("\\ncpu: "), std::string::npos);
  EXPECT_NE(dot.find("%)"), std::string::npos);
  EXPECT_NE(dot.find("n1 [label=\"1: child\\nstate: done\\nwait: "),
            std::string::npos);
  EXPECT_NE(dot.find("dag_start -> n0"), std::string::npos);
  EXPECT_NE(dot.find("n1 -> dag_end"), std::string::npos);
}
