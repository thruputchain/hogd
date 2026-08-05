#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <format>
#include <limits>
#include <print>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "event.hh"
#include "event_rpc.hh"
#include "hogd/base/coropoll.h"
#include "hogd/base/log.h"
#include "hogd/base/rpc_client.h"
#include "hogd/base/rpc_server.h"
#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h"
#include "hogd/crypto/crypto.h"
#include "hogd/hog_event/utilities.h"
#include "hogd/init.h"
#include "proposal.hh"
#include "rpc_types.hh"
#include "sched_event.hh"
#include "transaction.hh"
#include "types.hh"
#include "x23/async.h"
#include "x23/fastclock.h"
#include "x23/format.h"
#include "x23/serialize.h"
#include "x23/sockio.h"
#include "x23/vec.h"

namespace hogd {

namespace {

struct TestSchedEvent {
  using rpc_service = SchedEvent1;

  static EventPrepareResult schedevent_prepare_event(const EventCard &card)
  {
    std::println("Intro event server: {}", x23::show(card));
    EventPrepareResult result{EventPrepareOutcome::EventPrepareOK};
    result.snapshot().tip.block_id = std::numeric_limits<BlockID>::max();
    result.snapshot().tip.consensus_value.proposal_header.block_height = -1;
    result.snapshot().token = 1;
    return result;
  }

  static EventActivationResult
  schedevent_activate_event(const EventRegistrationSnapshot & /*unused*/)
  {
    return EventActivationResult{EventActivationOutcome::EventActivationOK};
  }
};

static_assert(is_SchedEvent1<TestSchedEvent *, std::type_identity_t>);

auto
connect_to_event_exec_server(const std::string &address)
{
  x23::HostPort host_port;
  if (!host_port.parse(address)) {
    throw std::runtime_error(
        std::format("Failed to parse event exec server address: {}", address));
  }
  auto client = make_rpc_client<EventExecutor1>(host_port, false)();
  if (!client)
    panic("connect_to_event_exec_server failed: {}",
          client.error().error_code().message());
  return std::move(*client);
}

auto
connect_to_event_sched_server(const std::string &address)
{
  x23::HostPort host_port;
  if (!host_port.parse(address)) {
    throw std::runtime_error(
        std::format("Failed to parse event sched server address: {}", address));
  }
  auto client = make_rpc_client<EventSched1>(host_port, false)();
  if (!client)
    panic("connect_to_event_sched_server failed: {}",
          client.error().error_code().message());
  return std::move(*client);
}

const SHA256Hash topic0 = sha256(std::string_view{"topic0"});
const SHA256Hash topic1 = sha256(std::string_view{"topic1"});
const SHA256Hash topic2 = sha256(std::string_view{"topic2"});

const std::vector<uint8_t> event_data = []() {
  std::vector<uint8_t> data(64);
  for (size_t i = 0; i < data.size(); i++) {
    data[i] = i;
  }
  return data;
}();

const ContractAddress contract_address =
    sha256(std::string_view{"contract_addr"});

ProposedTransaction
create_fake_body(size_t tx_id)
{
  ProposedTransaction body;
  body.signed_tx.tx.uniquifier.block_expiry_height = 1'000'000;
  body.signed_tx.tx.uniquifier.nonce = tx_id;
  body.signed_tx.tx.invocation.gas_limit = 1'000'000;
  return body;
}

// Create a fake TxEventLog with some events
TxEventLog
create_fake_event_log(size_t tx_id)
{
  TxEventLog event_log{};
  event_log.proposed_tx_hash = sha256(create_fake_body(tx_id).signed_tx.tx);

  // Create some fake events (3 events per transaction)
  for (size_t i = 0; i < 3; i++) {
    x23::Buf serialized_event;
    serialized_event.append_range(topic0);
    serialized_event.append_range(topic1);
    serialized_event.append_range(topic2);
    serialized_event.append_range(event_data);
    auto emitted_event =
        make_tx_emitted_event(contract_address, 3,
                              std::as_bytes(std::span(serialized_event)));
    event_log.events.emplace_back(std::move(emitted_event));
  }

  return event_log;
}

using event_exec_client_t = decltype(connect_to_event_exec_server(""));
using event_sched_client_t = decltype(connect_to_event_sched_server(""));

constexpr size_t kCoroBatchSize = 100;

x23::Detached
event_sender_coro(event_exec_client_t &exec_client, x23::CoroCB<void> &cb,
                  std::atomic<size_t> &event_offset, size_t total_events,
                  BlockID block_number, std::atomic<size_t> &n_remaining_coros)
{
  co_await default_threadpool().bg();

  while (true) {
    size_t start = event_offset.fetch_add(kCoroBatchSize);
    if (start >= total_events) {
      if (n_remaining_coros.fetch_sub(1, std::memory_order_relaxed) == 1) {
        cb();
      }
      co_return;
    }

    size_t end = std::min(start + kCoroBatchSize, total_events);

    for (size_t i = start; i < end; i++) {
      auto event_log = create_fake_event_log(i);
      auto body = create_fake_body(i);
      auto res = co_await exec_client.eventexecutor_emit_event(block_number, i,
                                                               event_log, body);
      if (res != RPCError::kRPCNone) {
        throw std::runtime_error(
            std::format("Failed to emit event {}: error code {}", i,
                        static_cast<int>(res)));
      }
    }
  }
}

void
run_event_bench(const std::string &exec_server_addr,
                const std::string &sched_server_addr, size_t num_events,
                size_t n_connections, size_t n_coros_per_connection,
                BlockID block_number)
{
  auto representative_event = create_fake_event_log(0);
  auto event_size = x23::serialize(representative_event).size();

  std::println("running event bench with {} events, {} connections, {} coros "
               "per connection, block number {}, event size {}",
               num_events, n_connections, n_coros_per_connection, block_number,
               event_size);
  // Connect to the event server
  auto event_sched_client = connect_to_event_sched_server(sched_server_addr);

  std::vector<event_exec_client_t> exec_clients;
  exec_clients.reserve(n_connections);
  for (size_t i = 0; i < n_connections; i++) {
    exec_clients.push_back(connect_to_event_exec_server(exec_server_addr));
  }

  auto total_start_time = x23::fastclock::get_ns();

  // Open block
  auto open_start = x23::fastclock::get_ns();
  auto res = event_sched_client.eventsched_open_block(block_number)();
  if (res != RPCError::kRPCNone) {
    throw std::runtime_error("Failed to open block");
  }
  auto res2 = exec_clients[0].eventexecutor_open_block(block_number)();
  if (res2 != RPCError::kRPCNone) {
    throw std::runtime_error("Failed to open executor event handle");
  }
  auto open_end = x23::fastclock::get_ns();
  double open_time = open_end - open_start;

  // Send events
  auto send_start = x23::fastclock::get_ns();

  std::atomic<size_t> event_offset = 0;
  std::atomic<size_t> n_total_coros = n_coros_per_connection * n_connections;

  {
    auto [async, cb] = x23::make_async<void>();
    for (auto &exec_client : exec_clients) {
      for (size_t i = 0; i < n_coros_per_connection; i++) {
        event_sender_coro(exec_client, cb, event_offset, num_events,
                          block_number, n_total_coros);
      }
    }
    async();
  }

  auto send_end = x23::fastclock::get_ns();
  double send_time = send_end - send_start;
  double events_per_sec = num_events * 1'000'000'000.0 / send_time;

  // Close block
  auto close_start = x23::fastclock::get_ns();
  // We use [block_number] as the height as well.
  auto res1 = exec_clients[0].eventexecutor_close_block(block_number)();
  if (res1 != RPCError::kRPCNone) {
    throw std::runtime_error("Failed to close executor event handle");
  }
  x23::XdrVec<SignedTransactionHash> hashes;
  x23::XdrVec<NondeterministicProposalData> nondeterministic_data;
  hashes.reserve(num_events);
  nondeterministic_data.reserve(num_events);
  for (size_t i = 0; i < num_events; ++i) {
    ProposedTransaction body = create_fake_body(i);
    hashes.push_back(sha256(body.signed_tx.tx));
    nondeterministic_data.push_back(body.nondeterministic_proposal_data);
  }
  ProposalTxs proposal_txs{
      .hash = t_to_parallel_batch(hashes)(),
      .nondeterministic_data = t_to_parallel_batch(nondeterministic_data)(),
  };
  auto close_res =
      event_sched_client.eventsched_close_block(block_number, block_number,
                                                proposal_txs)();
  if (close_res.error() != RPCError::kRPCNone) {
    throw std::runtime_error("Failed to close block");
  }
  auto close_end = x23::fastclock::get_ns();
  double close_time = close_end - close_start;

  auto total_end_time = x23::fastclock::get_ns();
  double total_time = total_end_time - total_start_time;
  double total_events_per_sec = num_events * 1'000'000'000.0 / total_time;

  std::println(
      "sent {} events in {} ns ({:.2f} inner events/sec) opened block in {} ns "
      "closed block in {} ns total time: {} ns ({:.2f} total events/sec)",
      num_events, send_time, events_per_sec, open_time, close_time, total_time,
      total_events_per_sec);
}

} // namespace

} // namespace hogd

namespace {
void
usage(char *argv[])
{
  std::println("Usage: {} -e <event_exec_server_addr> -s "
               "<event_sched_server_addr> [-n num_events] [-c num_connections] "
               "[-o coros_per_connection] [-b batch_size]",
               argv[0]);
  std::println("  -e: Event executor server address (e.g., hostname:8090)");
  std::println("  -s: Event scheduler server address (e.g., hostname:8091)");
  std::println("  -n: Number of events (default: 1000000)");
  std::println("  -c: Number of connections (default: 10)");
  std::println("  -o: Coroutines per connection (default: 100)");
  exit(EXIT_FAILURE);
}
} // namespace

int
main(int argc, char *argv[]) // NOLINT(bugprone-exception-escape)
{
  auto ret = hogd::hogd_init(argc, argv);
  if (ret)
    return ret;

  std::string exec_server_addr;
  std::string sched_server_addr;
  size_t num_events = 1'000'000;
  size_t num_connections = 20;
  size_t coros_per_connection = 100;

  int opt;
  // NOLINTBEGIN(misc-include-cleaner)
  while ((opt = getopt(argc, argv, "e:s:n:c:o:b:")) != -1) {
    switch (opt) {
    case 'e':
      exec_server_addr = optarg;
      break;
    case 's':
      sched_server_addr = optarg;
      break;
    case 'n':
      num_events = std::stoull(optarg);
      break;
    case 'c':
      num_connections = std::stoull(optarg);
      break;
    case 'o':
      coros_per_connection = std::stoull(optarg);
      break;
    default:
      usage(argv);
    }
  }
  // NOLINTEND(misc-include-cleaner)

  if (exec_server_addr.empty() || sched_server_addr.empty()) {
    std::println("Error: Both -e and -s arguments are required");
    usage(argv);
  }

  hogd::RpcServer<hogd::TestSchedEvent> test_sched_event_server;
  if (test_sched_event_server.start_server(8095) != 8095) {
    std::println("Error: Failed to start test sched event server");
    return EXIT_FAILURE;
  }

  for (size_t i = 0; i < 20; i++) {
    hogd::run_event_bench(exec_server_addr, sched_server_addr, num_events,
                          num_connections, coros_per_connection,
                          /*block_number=*/i);
  }

  std::fflush(stdout);
  return 0;
}
