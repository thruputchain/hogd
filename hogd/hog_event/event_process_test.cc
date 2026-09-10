#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
// NOLINTNEXTLINE(modernize-deprecated-headers)
#include <signal.h>
#include <unistd.h>
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
#include "hogd/base/xdr_util.h"
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
#include "transaction.hh"
#include "types.hh"
#include "x23/cleanup.h"
#include "x23/sockio.h"
#include "x23/vec.h"

#ifndef HOG_EVENT_BINARY
#error "HOG_EVENT_BINARY must be defined by CMake"
#endif

namespace hogd {
namespace {

ProposalTxs
empty_proposal_txs()
{
  x23::XdrVec<TransactionHash> hashes;
  x23::XdrVec<NondeterministicProposalData> nondeterministic_data;
  ProposalTxs proposal_txs;
  append_to_parallel_batch(hashes, proposal_txs.hash)();
  append_to_parallel_batch(nondeterministic_data,
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
           std::uint16_t exec_port, std::uint16_t indexer_port,
           std::uint16_t mempool_port, std::uint16_t metrics_port)
{
  return {"--mempool-addr",  mempool_addr,
          "--work-dir",      work_dir,
          "--bind-ip",       "127.0.0.1",
          "--executor-port", std::to_string(exec_port),
          "--indexer-port",  std::to_string(indexer_port),
          "--mempool-port",  std::to_string(mempool_port),
          "--metrics-port",  std::to_string(metrics_port)};
}

x23::UniqueFd
listen_loopback(std::uint16_t port)
{
  CHECK_NE(port, std::uint16_t{0});
  auto bind_addr = x23::HostPort{"127.0.0.1", port}.to_addr();
  CHECK(bind_addr.has_value());
  x23::UniqueFd listener = x23::stcplisten(*bind_addr);
  CHECK(static_cast<bool>(listener));
  return listener;
}

// exec, indexer, mempool, metrics -- in the order event_args() takes them.
constexpr std::size_t kEventPortCount = 4;
using EventPorts = std::array<std::uint16_t, kEventPortCount>;

struct PortWindow {
  std::uint16_t first;
  std::uint16_t last;
};

// The unprivileged ports the kernel never hands out on its own: everything
// below the ephemeral range, or above it when the range starts too low to
// leave room. connect() and bind(0) draw only from the ephemeral range, so a
// port outside it can be lost only to something that asked for it by number.
PortWindow
non_ephemeral_window()
{
  unsigned low = 0;
  unsigned high = 0;
  std::ifstream range("/proc/sys/net/ipv4/ip_local_port_range");
  if (!(range >> low >> high) || low < 1024 || high < low || high > 65535) {
    // Linux defaults, used when the sysctl is unreadable.
    low = 32768;
    high = 60999;
  }
  // A window of exactly kEventPortCount ports is enough, so both bounds are
  // inclusive: [kLowestUsable, low - 1] holds low - kLowestUsable ports, and
  // [high + 1, 65535] holds 65535 - high.
  constexpr unsigned kLowestUsable = 10000;
  if (low >= kLowestUsable + kEventPortCount) {
    return PortWindow{.first = static_cast<std::uint16_t>(kLowestUsable),
                      .last = static_cast<std::uint16_t>(low - 1)};
  }
  if (high + kEventPortCount <= 65535) {
    return PortWindow{.first = static_cast<std::uint16_t>(high + 1),
                      .last = 65535};
  }
  // The whole unprivileged space is ephemeral. Probe it anyway; there is
  // nowhere better to put the ports.
  return PortWindow{.first = 1024, .last = 65535};
}

std::optional<std::vector<x23::UniqueFd>>
try_hold_loopback_ports(const EventPorts &ports)
{
  std::vector<x23::UniqueFd> holders;
  holders.reserve(ports.size());
  for (const std::uint16_t port : ports) {
    try {
      holders.push_back(listen_loopback(port));
    } catch (const std::system_error &error) {
      // Only a collision means "try the next block". Anything else -- fd
      // exhaustion, a permission problem -- is an infrastructure failure, and
      // reporting it as a port hunt that ran out would bury it.
      if (error.code() != std::errc::address_in_use) {
        throw;
      }
      return std::nullopt;
    }
  }
  return holders;
}

std::vector<x23::UniqueFd>
hold_loopback_ports(const EventPorts &ports)
{
  std::optional<std::vector<x23::UniqueFd>> holders =
      try_hold_loopback_ports(ports);
  if (!holders) {
    throw std::runtime_error(
        std::format("lost reserved hog-event ports {}..{} while no hog-event "
                    "owned them",
                    ports.front(), ports.back()));
  }
  return std::move(*holders);
}

struct ReservedEventPorts {
  EventPorts ports;
  std::vector<x23::UniqueFd> holders;
};

// Reserves a block of consecutive loopback ports outside the ephemeral range,
// returning them along with the listeners that hold them.
ReservedEventPorts
reserve_event_ports()
{
  const PortWindow window = non_ephemeral_window();
  const unsigned span = static_cast<unsigned>(window.last - window.first) + 1;
  const unsigned slots = span / kEventPortCount;
  CHECK(slots > 0);
  // Two processes reserving at the same time must not converge on the same
  // block; the pid spreads them out and the probe settles the rest.
  unsigned slot = static_cast<unsigned>(getpid()) % slots;
  for (unsigned attempt = 0; attempt < std::min(slots, 512U); ++attempt) {
    const unsigned base = window.first + (slot * kEventPortCount);
    EventPorts ports{};
    for (std::size_t i = 0; i < kEventPortCount; ++i) {
      ports[i] = static_cast<std::uint16_t>(base + i);
    }
    if (std::optional<std::vector<x23::UniqueFd>> holders =
            try_hold_loopback_ports(ports);
        holders) {
      return ReservedEventPorts{.ports = ports, .holders = std::move(*holders)};
    }
    slot = (slot + 1) % slots;
  }
  throw std::runtime_error(
      std::format("no free block of {} loopback ports in {}..{}",
                  kEventPortCount, window.first, window.last));
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

  FailStopRpcServer<MempoolEventRpc> mempool_event;
  const std::uint16_t mempool_listen_port =
      mempool_event.start_server(x23::HostPort{"127.0.0.1", 0});
  const std::string mempool_addr =
      std::string(x23::HostPort{"127.0.0.1", mempool_listen_port}.show());

  TmpDir work_dir;
  // The EventCard advertises the addresses hog-event binds and prepare_event
  // rejects a card that changed, so the restart has to come back on the same
  // four ports. Reserve them up front, outside the ephemeral range, and hold
  // them with SO_REUSEADDR whenever no hog-event owns them: ctest runs this
  // test alongside one other per core, nearly all of them opening loopback
  // sockets, and a kernel-assigned ephemeral port is up for grabs the instant
  // the killed process releases it -- any connect() or bind(0) on the machine
  // can be handed it, and then the restart cannot rebind.
  ReservedEventPorts reserved = reserve_event_ports();
  const std::uint16_t event_exec_port = reserved.ports[0];
  const std::uint16_t event_indexer_port = reserved.ports[1];
  const std::uint16_t event_mempool_port = reserved.ports[2];
  const std::uint16_t event_metrics_port = reserved.ports[3];

  ConsensusValue block_one;
  {
    reserved.holders.clear();
    DaemonProcess event = DaemonProcess::start(
        HOG_EVENT_BINARY,
        event_args(mempool_addr, work_dir.directory(), event_exec_port,
                   event_indexer_port, event_mempool_port, event_metrics_port));
    event.wait_until_log_contains("event initialization complete.");
    ASSERT_EQ(event.require_port("event.exec"), event_exec_port);
    ASSERT_EQ(event.require_port("event.indexer"), event_indexer_port);
    ASSERT_EQ(event.require_port("event.mempool"), event_mempool_port);
    ASSERT_EQ(event.require_port("event.metrics"), event_metrics_port);

    {
      event_sched_client sched = connect_event_sched(event_mempool_port);
      block_one = commit_empty_block(sched, 11, 1, sha256(genesis));
      event_indexer_client indexer = connect_event_indexer(event_indexer_port);
      EventIndexerPullResult pulled =
          indexer.event_indexer_pull_next(uninitialized_pull())();
      ASSERT_EQ(pulled.outcome(),
                EventIndexerPullOutcome::EventIndexerPullBlock);
      EXPECT_EQ(pulled.pulled_block().block.payload.consensus_value, block_one);
    }

    registration.publish_event_canonical_tip(
        EventCanonicalTip{.block_id = 11, .consensus_value = block_one});

    EXPECT_EQ(event.kill_unclean(), SIGKILL);
    // Take the ports back until the restart execs.
    reserved.holders = hold_loopback_ports(reserved.ports);
  }

  {
    auto store =
        IndexerOutboxStore::open(work_dir.directory() + "/indexer-outbox");
    ASSERT_TRUE(store.has_value()) << store.error();
    auto committed = store->load_committed(1)();
    ASSERT_TRUE(committed.has_value()) << committed.error();
    EXPECT_EQ(committed->consensus_value, block_one);
  }

  reserved.holders.clear();
  DaemonProcess restarted =
      DaemonProcess::start(HOG_EVENT_BINARY,
                           event_args(mempool_addr, work_dir.directory(),
                                      event_exec_port, event_indexer_port,
                                      event_mempool_port, event_metrics_port));
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

  EXPECT_EQ(restarted.kill_unclean(), SIGKILL);
}

} // namespace
} // namespace hogd
