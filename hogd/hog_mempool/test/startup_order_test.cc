// Startup- and restart-ordering tests for the composed mempool node.
//
// hogd runs its subsystems as separate daemons, so nothing orders hog-auth's
// startup against the coordinator's registration. On a restart over a
// preserved database, hog-auth subscribes at the recovered height and
// immediately forwards that height to the mempool as an empty synchronization
// callback. That callback can land before the coordinator has installed the
// mempool's auth-data callback. Fresh databases report height -1 and send
// nothing, which is why a fresh-start smoke test never sees this.
//
// These tests compose the same components in-process and drive the orderings
// directly. The pre-ready cases are expected to fail until the mempool handles
// an auth-data update that arrives before coordinator readiness without
// terminating the process. See issue #2926.
//
// This lives in its own binary rather than in mempool_test so that a failure
// that aborts the process does not take the rest of the mempool suite with it.

#include <atomic>
#include <chrono>
#include <future>
#include <latch>
#include <memory>
#include <optional>
#include <string>
#include <thread>

#include <gtest/gtest.h>

#include "auth_rpc.hh"
#include "hogd/base/log.h"
#include "hogd/base/test.h"
#include "hogd/hog_mempool/rpc_from_auth.h"
#include "hogd/hog_mempool/rpc_from_coord.h"
#include "hogd/hog_mempool/test/infra.h"
#include "hogd/init.h"
#include "sched_coord.hh"
#include "types.hh"
#include "x23/async.h"

namespace hogd {

namespace {

// Launch a fresh node over [db_directory] and return the height it leaves
// committed. The node is torn down before this returns, so the directory holds
// exactly the state a restarting node would recover.
BlockHeight
commit_initial_chain(const std::string &db_directory)
{
  MempoolTestInfra node(MempoolTestInfraOptions{.db_directory = db_directory});
  const SchedulerGetTipResult tip =
      node.mempool_coord_rpc().schedcoord_get_tip()();
  CHECK_EQ(tip.outcome(), SchedulerGetTipOutcome::SchedulerGetTipOK);
  return tip.db_tip().value.proposal_header.block_height;
}

MempoolTestInfraOptions
restart_options(const std::string &db_directory,
                AuthStartupOrder auth_startup_order)
{
  return MempoolTestInfraOptions{
      .skip_genesis_block = true,
      .db_directory = db_directory,
      .recover_existing_chain = true,
      .auth_startup_order = auth_startup_order,
  };
}

} // namespace

// Control for the test below: the hog-devnode ordering, where the coordinator
// registers first and the mempool has installed its auth-data callback before
// hog-auth subscribes. This passes today, so a failure here means the
// recovered state or the harness is wrong rather than the ordering.
TEST(MempoolStartupOrder, RecoveredNodeStartsWhenAuthSubscribesAfterCoordinator)
{
  TmpDir db_dir;
  const BlockHeight committed = commit_initial_chain(db_dir.directory());
  ASSERT_GE(committed, 0);

  MempoolTestInfra restarted(
      restart_options(db_dir.directory(),
                      AuthStartupOrder::kSubscribeAfterCoordinator));
  EXPECT_EQ(restarted.mempool_coord_rpc().auth_data_height(), committed);
}

// The deployed restart ordering: hog-auth completes its intro and subscribes
// while the coordinator is still connecting. The database answers the
// subscription with the recovered tip, so hog-auth sends an auth-data update
// that reaches the mempool before the coordinator installs the callback.
//
// The mempool currently treats that as an invariant violation and aborts, so
// this test kills its own process. Recovering at the same height the control
// above reaches is the behavior to lock in.
TEST(MempoolStartupOrder,
     RecoveredNodeStartsWhenAuthSubscribesBeforeCoordinator)
{
  TmpDir db_dir;
  const BlockHeight committed = commit_initial_chain(db_dir.directory());
  ASSERT_GE(committed, 0);

  MempoolTestInfra restarted(
      restart_options(db_dir.directory(),
                      AuthStartupOrder::kSubscribeBeforeCoordinator));
  EXPECT_EQ(restarted.mempool_coord_rpc().auth_data_height(), committed);
}

// The same defect with no chain state involved. An auth peer that has
// completed its intro is free to send an auth-data update immediately, and the
// mempool must survive one that arrives before any callback is registered.
TEST(MempoolAuthStartupOrder, AuthDataChangedBeforeRegistrationIsNotFatal)
{
  MempoolAuthRpc auth_rpc;

  // hog-auth's synchronization callback on a preserved-state restart: the
  // recovered database height and an empty key-change batch.
  auth_rpc.mempoolauth_auth_data_changed(/*height=*/35, KeyChangeBatch{})();

  SUCCEED() << "auth-data update before callback registration did not "
               "terminate the mempool";
}

// Tolerating the pre-ready update must not cost later ones: once the
// coordinator has registered, updates have to reach it.
TEST(MempoolAuthStartupOrder, AuthDataChangedAfterRegistrationIsDelivered)
{
  MempoolAuthRpc auth_rpc;
  std::optional<BlockHeight> delivered_height;

  auth_rpc.register_callback(
      [&delivered_height](BlockHeight height,
                          KeyChangeBatch) -> x23::Async<void> {
        delivered_height = height;
        co_return;
      });
  auth_rpc.mempoolauth_auth_data_changed(/*height=*/36, KeyChangeBatch{})();

  EXPECT_EQ(delivered_height, 36);
}

// The teardown half of the same lifecycle gap. [mempoolauth_auth_data_changed]
// awaits through the callback stored in the auth RPC server, so destroying the
// server while a callback it dispatched is still running resumes that
// coroutine into freed memory. A restart hits this whenever hog-auth's
// synchronization callback overlaps mempool shutdown.
//
// The server destructor has to wait for callbacks it admitted. Today it does
// not, so the ordering assertion below fails; under ASAN the release that
// follows additionally reports a heap-use-after-free on the destroyed
// callback's closure.
TEST(MempoolAuthStartupOrder, TeardownWaitsForInFlightCallback)
{
  auto auth_rpc = std::make_unique<MempoolAuthRpc>();
  std::latch callback_running(1);
  std::latch release_callback(1);
  std::atomic<bool> callback_finished = false;

  auth_rpc->register_callback(
      [&](BlockHeight, KeyChangeBatch) -> x23::Async<void> {
        callback_running.count_down();
        release_callback.wait();
        callback_finished.store(true);
        co_return;
      });

  MempoolAuthRpc *dispatch_target = auth_rpc.get();
  std::thread deliver([dispatch_target] {
    dispatch_target->mempoolauth_auth_data_changed(/*height=*/7,
                                                   KeyChangeBatch{})();
  });
  callback_running.wait();

  std::promise<void> destroyed;
  std::future<void> teardown_finished = destroyed.get_future();
  std::thread teardown([&] {
    auth_rpc.reset();
    destroyed.set_value();
  });

  // We are still holding the callback, so the destructor must not have
  // returned.
  EXPECT_EQ(teardown_finished.wait_for(std::chrono::milliseconds(200)),
            std::future_status::timeout);

  release_callback.count_down();
  deliver.join();
  teardown.join();
  EXPECT_TRUE(callback_finished.load());
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
