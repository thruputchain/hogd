#include <gtest/gtest.h>

#include <cstdint>
#include <span>
#include <string>
#include <sys/wait.h>
// NOLINTNEXTLINE(modernize-deprecated-headers)
#include <signal.h>
#include <utility>
#include <vector>

#include "consensus.hh"
#include "event_rpc.hh"
#include "hogd/base/daemon_process_test.h"
#include "hogd/base/log.h"
#include "hogd/base/rpc_client.h"
#include "hogd/base/rpc_message_size.h"
#include "hogd/base/rpc_server.h"
#include "hogd/base/test.h"
#include "hogd/crypto/crypto.h"
#include "hogd/hog_event/indexer_outbox.h"
#include "hogd/hog_mempool/registration.h"
#include "hogd/hog_mempool/rpc_from_event.h"
#include "hogd/version.h"
#include "hogd/xdr/indexer_payload_validation.h"
#include "indexer.hh"
#include "proposal.hh"
#include "proposal_hash.h"
#include "rpc_types.hh"
#include "sched_event.hh"
#include "types.hh"
#include "x23/cleanup.h"
#include "x23/vec.h"

#ifndef HOG_EVENT_BINARY
#error "HOG_EVENT_BINARY must be defined by CMake"
#endif

namespace hogd {
namespace {

ProposalTxs
empty_proposal_txs()
{
  ProposalTxs proposal_txs;
  append_to_parallel_batch(x23::span<const TransactionHash>({}),
                           proposal_txs.hash)();
  append_to_parallel_batch(x23::span<const NondeterministicProposalData>({}),
                           proposal_txs.nondeterministic_data)();
  return proposal_txs;
}

ConsensusValue
make_consensus_value(BlockHeight height, SHA256Hash event_root)
{
  ProposalTxs proposal_txs = empty_proposal_txs();
  ConsensusValue value{};
  value.proposal_header.block_height = height;
  value.proposal_header.events_hash = event_root;
  auto txs_root = compute_indexer_proposal_txs_root(proposal_txs)();
  CHECK(txs_root.has_value());
  value.proposal_header.txs_hash = *txs_root;
  value.proposal_txs_hash = proposal_txs_compatibility_hash(proposal_txs);
  return value;
}

EventIndexerPullRequest
uninitialized_pull()
{
  return {.protocol = version::kHogdProtocol(),
          .progress = IndexerProgress(
              IndexerProgressState::IndexerProgressUninitialized)};
}

IndexerProgress
progress_at(const EventIndexerPulledBlock &pulled)
{
  IndexerProgress progress(IndexerProgressState::IndexerProgressInitialized);
  progress.initialized() = {
      .origin = pulled.origin,
      .contiguous_height =
          pulled.block.payload.consensus_value.proposal_header.block_height,
      .last_consensus_hash = pulled.block.identity.consensus_hash,
  };
  return progress;
}

using event_sched_client = rpc_client_t<EventSched1>;
using event_indexer_client = rpc_client_t<EventIndexer1>;

event_sched_client
connect_event_sched(std::uint16_t port)
{
  auto client =
      make_rpc_client<EventSched1>(x23::HostPort{"127.0.0.1", port},
                                   /*retry=*/true,
                                   kEventSchedulerRpcMaxMessageSize)();
  CHECK(client.has_value());
  return std::move(*client);
}

event_indexer_client
connect_event_indexer(std::uint16_t port)
{
  auto client = make_rpc_client<EventIndexer1>(x23::HostPort{"127.0.0.1", port},
                                               /*retry=*/true,
                                               kIndexerRpcMaxMessageSize)();
  CHECK(client.has_value());
  return std::move(*client);
}

ConsensusValue
commit_empty_block(event_sched_client &rpc, BlockID block_id,
                   BlockHeight height, ConsensusValueHash prev_hash)
{
  CHECK_EQ(rpc.eventsched_open_block(block_id)(), RPCError::kRPCNone);
  EventBlockResult closed = rpc.eventsched_close_block(block_id, height)();
  CHECK_EQ(closed.error(), RPCError::kRPCNone);
  ConsensusValue value = make_consensus_value(height, closed.root_hash());
  value.prev_hash = std::move(prev_hash);
  CHECK_EQ(rpc.eventsched_commit_block(block_id, value)(), RPCError::kRPCNone);
  return value;
}

std::vector<std::string>
event_args(const std::string &mempool_addr, const std::string &work_dir,
           const std::string &exec_port, const std::string &indexer_port,
           const std::string &mempool_port, const std::string &metrics_port)
{
  return {"--mempool-addr", mempool_addr, "--work-dir",      work_dir,
          "--bind-ip",      "127.0.0.1",  "--executor-port", exec_port,
          "--indexer-port", indexer_port, "--mempool-port",  mempool_port,
          "--metrics-port", metrics_port};
}

TEST(HogEventProcess, RecoversCommittedOutboxAfterUncleanKill)
{
  Registration &registration = Registration::registration();
  registration.clear();
  x23::Defer cleanup([&registration] { registration.clear(); });

  ConsensusValue genesis{};
  genesis.proposal_header.block_height = 0;
  registration.publish_event_canonical_tip(
      EventCanonicalTip{.block_id = 0, .consensus_value = genesis});

  RpcServer<MempoolEventRpc> mempool_event;
  const std::uint16_t mempool_listen_port =
      mempool_event.start_server(x23::HostPort{"127.0.0.1", 0});
  const std::string mempool_addr =
      std::string(x23::HostPort{"127.0.0.1", mempool_listen_port}.show());

  TmpDir work_dir;
  std::uint16_t event_mempool_port = 0;
  std::uint16_t event_indexer_port = 0;
  std::uint16_t event_exec_port = 0;
  std::uint16_t event_metrics_port = 0;
  ConsensusValue block_one;
  {
    DaemonProcess event =
        DaemonProcess::start(HOG_EVENT_BINARY,
                             event_args(mempool_addr, work_dir.directory(), "0",
                                        "0", "0", "0"));
    event.wait_until_log_contains("event initialization complete.");
    event_exec_port = event.require_port("event.exec");
    event_indexer_port = event.require_port("event.indexer");
    event_mempool_port = event.require_port("event.mempool");
    event_metrics_port = event.require_port("event.metrics");

    event_sched_client sched = connect_event_sched(event_mempool_port);
    block_one = commit_empty_block(sched, 11, 1, sha256(genesis));
    event_indexer_client indexer = connect_event_indexer(event_indexer_port);
    EventIndexerPullResult pulled =
        indexer.event_indexer_pull_next(uninitialized_pull())();
    ASSERT_EQ(pulled.outcome(), EventIndexerPullOutcome::EventIndexerPullBlock);
    EXPECT_EQ(pulled.pulled_block().block.payload.consensus_value, block_one);

    registration.publish_event_canonical_tip(
        EventCanonicalTip{.block_id = 11, .consensus_value = block_one});

    const int status = event.kill_unclean();
    ASSERT_TRUE(WIFSIGNALED(status));
    EXPECT_EQ(WTERMSIG(status), SIGKILL);
  }

  {
    auto store =
        IndexerOutboxStore::open(work_dir.directory() + "/indexer-outbox");
    ASSERT_TRUE(store.has_value()) << store.error();
    auto committed = store->load_committed(1)();
    ASSERT_TRUE(committed.has_value()) << committed.error();
    EXPECT_EQ(committed->consensus_value, block_one);
  }

  DaemonProcess restarted =
      DaemonProcess::start(HOG_EVENT_BINARY,
                           event_args(mempool_addr, work_dir.directory(),
                                      std::to_string(event_exec_port),
                                      std::to_string(event_indexer_port),
                                      std::to_string(event_mempool_port),
                                      std::to_string(event_metrics_port)));
  restarted.wait_until_log_contains("event initialization complete.");

  event_indexer_client indexer = connect_event_indexer(event_indexer_port);
  EventIndexerPullResult first =
      indexer.event_indexer_pull_next(uninitialized_pull())();
  ASSERT_EQ(first.outcome(), EventIndexerPullOutcome::EventIndexerPullBlock);
  EXPECT_EQ(first.pulled_block().block.payload.consensus_value, block_one);

  event_sched_client sched = connect_event_sched(event_mempool_port);
  ConsensusValue block_two =
      commit_empty_block(sched, 12, 2, sha256(block_one));
  EventIndexerPullRequest next_request{.protocol = version::kHogdProtocol(),
                                       .progress =
                                           progress_at(first.pulled_block())};
  EventIndexerPullResult second =
      indexer.event_indexer_pull_next(next_request)();
  ASSERT_EQ(second.outcome(), EventIndexerPullOutcome::EventIndexerPullBlock);
  EXPECT_EQ(second.pulled_block().block.payload.consensus_value, block_two);

  const int status = restarted.kill_unclean();
  ASSERT_TRUE(WIFSIGNALED(status));
  EXPECT_EQ(WTERMSIG(status), SIGKILL);
}

} // namespace
} // namespace hogd
