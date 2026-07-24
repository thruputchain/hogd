#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <format>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "event.hh"
#include "hogd/base/types.h"
#include "hogd/crypto/crypto.h"
#include "hogd/event/utilities.h"
#include "hogd/indexer/db.h"
#include "hogd/init.h"
#include "types.hh"
#include "x23/fastclock.h"
#include "x23/vec.h"

namespace hogd {

namespace {

using namespace std::string_view_literals;

// NOLINTBEGIN(bugprone-throwing-static-initialization)
const SHA256Hash topic0 = sha256("topic0"sv);
const SHA256Hash topic1 = sha256("topic1"sv);
const SHA256Hash topic2 = sha256("topic2"sv);
// NOLINTEND(bugprone-throwing-static-initialization)

auto make_event_data = [](uint32_t val) {
  x23::Buf data;
  data.append_range(sha256(val + 1));
  data.append_range(sha256(val + 2));
  return data;
};

// Create a fake TxEventLog with events from multiple contract addresses
TxEventLog
create_fake_event_log(uint32_t tx_id, size_t num_events_per_tx,
                      std::span<const ContractAddress> contract_addresses)
{
  TxEventLog event_log{};
  // Use tx_id to create a unique hash
  event_log.proposed_tx_hash = sha256(tx_id);

  // Create events from different contract addresses
  for (size_t i = 0; i < num_events_per_tx; i++) {
    const ContractAddress &contract_addr =
        contract_addresses[i % contract_addresses.size()];

    x23::Buf serialized_event;
    serialized_event.append_range(topic0);
    serialized_event.append_range(topic1);
    serialized_event.append_range(topic2);
    serialized_event.append_range(make_event_data(tx_id));

    auto emitted_event =
        make_tx_emitted_event(contract_addr, 3,
                              std::as_bytes(std::span(serialized_event)));
    event_log.events.emplace_back(std::move(emitted_event));
  }

  return event_log;
}

// Generate contract addresses from account IDs
std::vector<ContractAddress>
generate_contract_addresses(size_t num_accounts)
{
  std::vector<ContractAddress> addresses;
  addresses.reserve(num_accounts);

  for (size_t i = 0; i < num_accounts; i++) {
    std::string account_str = std::format("account_{}", i);
    addresses.push_back(sha256(std::string_view{account_str}));
  }

  return addresses;
}

[[noreturn]] void
usage(const char *progname, int exit_code = EXIT_FAILURE)
{
  std::println(stderr, "Usage: {} -d <db_path> [OPTIONS]", progname);
  std::println(stderr, "Options:");
  std::println(stderr, "  -d <path>     Database path (required)");
  std::println(stderr, "  -a <num>      Number of accounts (default: 10)");
  std::println(stderr, "  -e <num>      Events per transaction (default: 3)");
  std::println(stderr, "  -b <num>      Number of blocks (default: 10)");
  std::println(stderr, "  -t <num>      Transactions per block (default: 100)");
  std::println(stderr, "  -h            Show this help message");
  exit(exit_code);
}

} // namespace

} // namespace hogd

int
main(int argc, char *argv[]) // NOLINT(bugprone-exception-escape)
{
  auto ret = hogd::hogd_init(argc, argv, false);
  if (ret != 0)
    return ret;

  // Default values
  std::string db_path;
  size_t num_accounts = 10;
  size_t num_events_per_tx = 3; // Match event server bench default
  int num_blocks = 10;
  size_t txs_per_block = 100;

  // Parse command line arguments
  int opt;
  // NOLINTBEGIN(misc-include-cleaner)
  while ((opt = getopt(argc, argv, "d:a:e:b:t:h")) != -1) {
    switch (opt) {
    case 'd':
      db_path = optarg;
      break;
    case 'a':
      num_accounts = std::stoull(optarg);
      break;
    case 'e':
      num_events_per_tx = std::stoull(optarg);
      break;
    case 'b':
      num_blocks = std::stoull(optarg);
      break;
    case 't':
      txs_per_block = std::stoull(optarg);
      break;
    case 'h':
      hogd::usage(argv[0], EXIT_SUCCESS);
      break;
    default:
      hogd::usage(argv[0]);
      break;
    }
  }

  // Check for required arguments
  if (db_path.empty()) {
    std::println(stderr, "Error: Database path (-d) is required");
    hogd::usage(argv[0]);
  }

  // Check for extra arguments
  if (optind != argc) {
    std::println(stderr, "Error: Unexpected extra arguments");
    hogd::usage(argv[0]);
  }
  // NOLINTEND(misc-include-cleaner)

  std::println("Indexer benchmark:");
  std::println("  Database path: {}", db_path);
  std::println("  Number of accounts: {}", num_accounts);
  std::println("  Events per transaction: {}", num_events_per_tx);
  std::println("  Number of blocks: {}", num_blocks);
  std::println("  Transactions per block: {}", txs_per_block);
  std::println("  Total transactions: {}", num_blocks * txs_per_block);
  std::println("  Total events: {}",
               (num_blocks * txs_per_block) * num_events_per_tx);

  // Generate contract addresses
  auto contract_addresses = hogd::generate_contract_addresses(num_accounts);

  // Remove existing database if it exists
  std::filesystem::remove_all(db_path);

  // Create new database
  auto db_result = hogd::IndexDB::make_new(db_path);
  if (!db_result.has_value()) {
    std::println(stderr, "Failed to create database");
    return 1;
  }
  hogd::IndexDB db = std::move(*db_result);

  // Generate all event logs
  std::println("Generating event data...");
  auto gen_start = x23::fastclock::get_ns();
  std::vector<std::vector<hogd::TxEventLog>> blocks(num_blocks);
  for (int block = 0; block < num_blocks; block++) {
    blocks[block].reserve(txs_per_block);
    for (size_t tx = 0; tx < txs_per_block; tx++) {
      size_t tx_id = (block * txs_per_block) + tx;
      blocks[block].push_back(hogd::create_fake_event_log(tx_id,
                                                          num_events_per_tx,
                                                          contract_addresses));
    }
  }
  auto gen_end = x23::fastclock::get_ns();
  double gen_time = (gen_end - gen_start) / 1'000'000'000.0;
  std::println("Generated event data in {:.3f} seconds", gen_time);

  // Insert events into database
  std::println("Inserting events into database...");
  auto insert_start = x23::fastclock::get_ns();

  for (hogd::BlockHeight block = 0; block < num_blocks; block++) {
    auto result = db.consume_events(block, blocks[block], {});
    if (!result.has_value()) {
      std::println(stderr, "Failed to consume events for block {}", block);
      return 1;
    }
    db.log_stats();
  }

  auto insert_end = x23::fastclock::get_ns();
  double insert_time = (insert_end - insert_start) / 1'000'000'000.0;
  size_t total_events = (num_blocks * txs_per_block) * num_events_per_tx;
  double events_per_sec = total_events / insert_time;

  std::println("Results:");
  std::println("  Insert time: {:.3f} seconds", insert_time);
  std::println("  Total events inserted: {}", total_events);
  std::println("  Events per second: {:.0f}", events_per_sec);
  std::println("  Transactions per second: {:.0f}",
               (num_blocks * txs_per_block) / insert_time);

  return 0;
}
