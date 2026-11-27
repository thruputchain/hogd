// Measures strict block apply (IndexDB::apply_committed_block), the path
// production uses, with large blocks and optional reopens between blocks.
// Block generation is not timed. The -d directory must not exist yet.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <print>
#include <string>
#include <unistd.h>
#include <utility>
#include <vector>

#include "event.hh"
#include "hogd/base/xdr_util.h"
#include "hogd/crypto/crypto.h"
#include "hogd/hog_indexer/db.h"
#include "hogd/init.h"
#include "hogd/xdr/indexer_payload_validation.h"
#include "hogd/xdr/proposal_hash.h"
#include "hogd/xdr/protocol_token.h"
#include "indexer.hh"
#include "proposal.hh"
#include "transaction.hh"
#include "x23/fastclock.h"

namespace hogd {
namespace {

struct BenchOptions {
  std::string db_path;
  int blocks = 20;
  size_t txs_per_block = 100'000;
  size_t addresses = 1000;
  size_t calldata_bytes = 128;
  size_t event_bytes = 96;
  int reopen_every = 0;
};

ProposedTransaction
make_body(BlockHeight height, uint64_t nonce, const BenchOptions &opts)
{
  ProposedTransaction body{};
  auto &tx = body.signed_tx.tx;
  tx.uniquifier.block_expiry_height = height;
  tx.uniquifier.nonce = nonce;
  const uint64_t address = nonce % opts.addresses;
  std::ranges::fill(tx.invocation.invoked_address, 0);
  std::copy_n(reinterpret_cast<const uint8_t *>(&address), sizeof(address),
              tx.invocation.invoked_address.begin());
  tx.invocation.calldata.resize(opts.calldata_bytes,
                                static_cast<uint8_t>(nonce));
  return body;
}

TxEventLog
make_log(const ProposedTransaction &body, const BenchOptions &opts)
{
  TxEventLog log{};
  log.tx_hash = sha256(body.signed_tx.tx);
  TxEmittedEvent event{};
  event.contract_address = body.signed_tx.tx.invocation.invoked_address;
  event.serialized_event.resize(opts.event_bytes, 0xEE);
  log.events.push_back(std::move(event));
  return log;
}

IndexerCommittedBlock
make_block(BlockHeight height, const ConsensusValueHash &prev_hash,
           const BenchOptions &opts)
{
  ProposedTransactionBatch bodies;
  TxEventLogBatch logs;
  SignedTransactionHashBatch signed_hashes;
  x23::XdrVec<NondeterministicProposalData> nondeterministic_data;
  for (size_t i = 0; i < opts.txs_per_block; ++i) {
    const uint64_t nonce =
        (static_cast<uint64_t>(height) * opts.txs_per_block) + i;
    bodies.push_back(make_body(height, nonce, opts));
    logs.push_back(make_log(bodies.back(), opts));
    signed_hashes.push_back(sha256(bodies.back().signed_tx));
    nondeterministic_data.push_back(
        bodies.back().nondeterministic_proposal_data);
  }
  ProposalTxs proposal;
  proposal.signed_hash = t_to_parallel_batch(signed_hashes)();
  proposal.nondeterministic_data = t_to_parallel_batch(nondeterministic_data)();

  ConsensusValue value{};
  value.prev_hash = prev_hash;
  value.proposal_header.block_height = height;
  value.proposal_header.block_timepoint = 1000 + height;
  value.proposal_header.events_hash =
      compute_indexer_outbox_event_root(logs, bodies);
  auto txs_root = compute_indexer_txs_root(bodies);
  if (!txs_root) {
    std::println(stderr, "txs root failed: {}", txs_root.error());
    std::exit(1);
  }
  value.proposal_header.txs_hash = *txs_root;
  value.proposal_txs_hash = proposal_txs_compatibility_hash(proposal);
  IndexerCommittedBlockPayload payload{
      .format_version = kIndexerOutboxFormatVersion,
      .consensus_value = std::move(value),
      .event_logs = std::move(logs),
      .tx_bodies = std::move(bodies),
  };
  auto block = make_indexer_committed_block(payload)();
  if (!block) {
    std::println(stderr, "block build failed: {}", block.error());
    std::exit(1);
  }
  return std::move(*block);
}

IndexDB
open_db(const BenchOptions &opts)
{
  auto db = IndexDB::open_or_make_new(opts.db_path);
  if (!db) {
    std::println(stderr, "failed to open {}", opts.db_path);
    std::exit(1);
  }
  return std::move(*db);
}

[[noreturn]] void
usage(const char *progname, int exit_code = EXIT_FAILURE)
{
  std::println(exit_code == EXIT_SUCCESS ? stdout : stderr,
               "usage: {} -d DB_PATH [-b BLOCKS] [-t TXS_PER_BLOCK] "
               "[-a ADDRESSES] [-c CALLDATA_BYTES] [-e EVENT_BYTES] "
               "[-r REOPEN_EVERY_BLOCKS]",
               progname);
  std::exit(exit_code);
}

int
run(const BenchOptions &opts)
{
  if (std::filesystem::exists(opts.db_path)) {
    std::println(stderr, "{} already exists; pass a new path", opts.db_path);
    return 1;
  }
  ConsensusValueHash genesis;
  std::ranges::fill(genesis, 0x42);
  const IndexerOutboxOrigin origin{
      .protocol = protocol_from_text("hogp/bench"),
      .epoch_sequence = 0,
      .first_height = 1,
      .first_prev_hash = genesis,
  };

  std::optional<IndexDB> db(open_db(opts));
  if (db->initialize_epoch(origin).outcome() !=
      IndexerEpochOutcome::IndexerEpochInitialized) {
    std::println(stderr, "epoch initialization failed");
    return 1;
  }

  ConsensusValueHash prev = genesis;
  double total_apply = 0;
  for (BlockHeight height = 1; height <= opts.blocks; ++height) {
    if (opts.reopen_every > 0 && height > 1 &&
        (height - 1) % opts.reopen_every == 0) {
      db.reset();
      db.emplace(open_db(opts));
    }
    IndexerCommittedBlock block = make_block(height, prev, opts);
    prev = block.identity.consensus_hash;
    const auto start = x23::fastclock::now();
    const IndexerBlockApplyResult applied =
        db->apply_committed_block(origin, std::move(block))();
    const double seconds =
        static_cast<double>((x23::fastclock::now() - start).count()) / 1e9;
    if (applied.outcome() != IndexerBlockApplyOutcome::IndexerBlockApplied) {
      std::println(stderr, "apply of block {} failed: {}", height,
                   applied.outcome());
      return 1;
    }
    total_apply += seconds;
    std::println("block {} apply {:.3f} s", height, seconds);
  }
  std::println("total apply {:.3f} s, {:.0f} tx/s", total_apply,
               static_cast<double>(opts.blocks * opts.txs_per_block) /
                   total_apply);
  return 0;
}

} // namespace
} // namespace hogd

int
main(int argc, char *argv[]) // NOLINT(bugprone-exception-escape)
{
  if (int ret = hogd::hogd_init(argc, argv, false); ret != 0) {
    return ret;
  }
  hogd::BenchOptions opts;
  int opt;
  // NOLINTBEGIN(misc-include-cleaner)
  while ((opt = getopt(argc, argv, "d:b:t:a:c:e:r:h")) != -1) {
    switch (opt) {
    case 'd':
      opts.db_path = optarg;
      break;
    case 'b':
      opts.blocks = std::stoi(optarg);
      break;
    case 't':
      opts.txs_per_block = std::stoull(optarg);
      break;
    case 'a':
      opts.addresses = std::stoull(optarg);
      break;
    case 'c':
      opts.calldata_bytes = std::stoull(optarg);
      break;
    case 'e':
      opts.event_bytes = std::stoull(optarg);
      break;
    case 'r':
      opts.reopen_every = std::stoi(optarg);
      break;
    case 'h':
      hogd::usage(argv[0], EXIT_SUCCESS);
      break;
    default:
      hogd::usage(argv[0]);
      break;
    }
  }
  // NOLINTEND(misc-include-cleaner)
  if (opts.db_path.empty() || optind != argc || opts.blocks <= 0 ||
      opts.txs_per_block == 0 || opts.addresses == 0 ||
      opts.event_bytes > hogd::kMaxEventLength) {
    hogd::usage(argv[0]);
  }
  return hogd::run(opts);
}
