#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <format>
#include <span>
#include <stdexcept>
#include <string>
#include <sys/wait.h>
// NOLINTNEXTLINE(modernize-deprecated-headers)
#include <signal.h>
#include <tuple>
#include <utility>
#include <vector>

#include "consensus.hh"
#include "event_rpc.hh"
#include "hogd/base/daemon_process_test.h"
#include "hogd/base/log.h"
#include "hogd/base/rpc_client.h"
#include "hogd/base/rpc_server.h"
#include "hogd/base/test.h"
#include "hogd/crypto/crypto.h"
#include "hogd/hog_event/rpc_context.h"
#include "hogd/hog_event/rpc_from_indexer.h"
#include "hogd/hog_event/rpc_from_sched.h"
#include "hogd/hog_indexer/db.h"
#include "hogd/xdr/indexer_payload_validation.h"
#include "indexer.hh"
#include "network.hh"
#include "pagination.hh"
#include "proposal.hh"
#include "proposal_hash.h"
#include "rpc_types.hh"
#include "types.hh"
#include "x23/vec.h"

#ifndef HOG_INDEXER_BINARY
#error "HOG_INDEXER_BINARY must be defined by CMake"
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

ConsensusValue
commit_empty_block(EventRPCContext &ctx, BlockID block_id, BlockHeight height,
                   ConsensusValueHash prev_hash)
{
  EventMempoolRpc rpc(ctx);
  CHECK_EQ(rpc.eventsched_open_block(block_id)(), RPCError::kRPCNone);
  EventBlockResult closed = rpc.eventsched_close_block(block_id, height)();
  CHECK_EQ(closed.error(), RPCError::kRPCNone);
  ConsensusValue value = make_consensus_value(height, closed.root_hash());
  value.prev_hash = std::move(prev_hash);
  CHECK_EQ(rpc.eventsched_commit_block(block_id, value)(), RPCError::kRPCNone);
  return value;
}

BlockHeight
query_latest_block(std::uint16_t port)
{
  auto client =
      make_rpc_client<IndexerBlockExplorer1>(x23::HostPort{"127.0.0.1", port},
                                             /*retry=*/true)();
  if (!client.has_value()) {
    throw std::runtime_error(
        std::format("indexer block-explorer connect failed: {}",
                    client.error().error_code().message()));
  }
  AddressTransactionQuery query{};
  query.order = IterationOrder::IterationOrderAscending;
  query.anchor.kind(BlockAnchorKind::AnchorAtBeginning);
  return client->get_transactions_by_address(query)().latest_block;
}

TEST(HogIndexerProcess, RecoversAfterUncleanKillAndResumesPull)
{
  TmpDir event_dir;
  TmpDir indexer_dir;
  const std::filesystem::path indexer_path =
      std::filesystem::path(indexer_dir.directory()) / "indexer-db";

  ConsensusValue genesis{};
  genesis.proposal_header.block_height = 0;
  EventRPCContext event(event_dir.directory());
  ASSERT_TRUE(event.reconcile_outbox_tip(0, genesis)().has_value());
  EventIndexerServer event_server(event);
  const std::uint16_t event_port =
      event_server.start_server(x23::HostPort{"127.0.0.1", 0});
  const std::string event_addr =
      std::string(x23::HostPort{"127.0.0.1", event_port}.show());
  ConsensusValue block_one = commit_empty_block(event, 1, 1, sha256(genesis));

  std::uint16_t explorer_port = 0;
  {
    DaemonProcess indexer =
        DaemonProcess::start(HOG_INDEXER_BINARY,
                             {"--event-addr", event_addr, "--db-path",
                              indexer_path.string(), "--bind-ip", "127.0.0.1",
                              "--block-explorer-port", "0", "--metrics-port",
                              "0"});
    indexer.wait_until_log_contains("indexer initialization complete.");
    explorer_port = indexer.require_port("indexer.block_explorer");
    EXPECT_EQ(query_latest_block(explorer_port), 1);

    const int status = indexer.kill_unclean();
    ASSERT_TRUE(WIFSIGNALED(status));
    EXPECT_EQ(WTERMSIG(status), SIGKILL);
  }

  {
    auto db = IndexDB::open(indexer_path);
    ASSERT_TRUE(db.has_value()) << db.error();
    EXPECT_EQ(db->latest_block(), 1);
    const IndexerProgress progress = db->durable_progress();
    ASSERT_EQ(progress.state(),
              IndexerProgressState::IndexerProgressInitialized);
    EXPECT_EQ(progress.initialized().contiguous_height, 1);
    EXPECT_EQ(progress.initialized().last_consensus_hash, sha256(block_one));
  }

  std::ignore = commit_empty_block(event, 2, 2, sha256(block_one));

  DaemonProcess restarted =
      DaemonProcess::start(HOG_INDEXER_BINARY,
                           {"--event-addr", event_addr, "--db-path",
                            indexer_path.string(), "--bind-ip", "127.0.0.1",
                            "--block-explorer-port", "0", "--metrics-port",
                            "0"});
  restarted.wait_until_log_contains("indexer initialization complete.");
  explorer_port = restarted.require_port("indexer.block_explorer");
  EXPECT_EQ(query_latest_block(explorer_port), 2);

  const int status = restarted.kill_unclean();
  ASSERT_TRUE(WIFSIGNALED(status));
  EXPECT_EQ(WTERMSIG(status), SIGKILL);
}

} // namespace
} // namespace hogd
