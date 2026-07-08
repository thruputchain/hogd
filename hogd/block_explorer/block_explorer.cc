#include <cstdlib>
#include <format>
#include <optional>
#include <print>
#include <string>
#include <unistd.h> // NOLINT(misc-include-cleaner)

#include "hogd/base/log.h"
#include "hogd/base/rpc_client.h"
#include "hogd/base/rpc_server.h"
#include "hogd/block_explorer/shared.h"
#include "hogd/container/merkle/hex_array.h"
#include "hogd/init.h"
#include "hogd/loadgen/loadgen_server.h"
#include "hogd/version.h"
#include "indexer.hh"
#include "types.hh"

namespace hogd {

namespace {

void
print_usage()
{
  std::println("usage: <indexer_server:port> [-t <tx_hash>]");
}

int
block_explorer_init(const std::string &indexer_server_addr,
                    const std::optional<std::string> &tx_hash_raw)
{
  if (!tx_hash_raw) {
    print_usage();
    return EXIT_FAILURE;
  }

  // Register with the indexer server.
  indexer_block_explorer_client_t client =
      make_rpc_client<IndexerBlockExplorer1>(server_address_to_sockaddr(
                                                 indexer_server_addr),
                                             /*retry=*/true,
                                             LoadgenRpc::kRpcMaxMessageSize)();
  const IndexerBlockExplorerIntroOutcome intro_result =
      client.indexer_block_explorer_intro(version::kHogdProtocol())();
  if (intro_result != IndexerBlockExplorerIntroOutcome::IntroSuccess) {
    info("block explorer failed to connect to indexer (error: {})",
         intro_result);
    return EXIT_FAILURE;
  }

  SHA256Hash tx_hash = HexArray(*tx_hash_raw).to_sha256();
  info("parsed tx_hash is {}", HexArray(*tx_hash_raw).str());
  TxInfoResult tx_info_res = client.get_transaction_info(tx_hash)();
  if (tx_info_res.outcome() == TxInfoOutcome::TxInfoFound) {
    const TxInfo &tx_info = tx_info_res.info();
    info("tx info:");
    info("hash = {}", *tx_hash_raw);
    info("block_height = {}", tx_info.height);
  }
  else if (tx_info_res.outcome() == TxInfoOutcome::TxInfoRecentFailure) {
    // Best-effort diagnostic for a tx that failed nondeterministically at
    // proposal time and never committed (expires after a few blocks).
    const TxFailureDiagnostic &diag = tx_info_res.recent_failure();
    info("tx recently failed (nondeterministic conflict; not committed):");
    info("hash = {}", *tx_hash_raw);
    info("attempted at block_height = {}", diag.attempt_height);
    info("conflict code = {}", x23::show(diag.conflict.code));
  }
  else {
    info("could not find tx (hash = {}).", *tx_hash_raw);
  }

  return 0;
}

} // namespace

} // namespace hogd

// NOLINTBEGIN(bugprone-exception-escape)
int
main(int argc, char *argv[])
{
  int ret = hogd::hogd_init(argc, argv);
  if (ret) {
    return ret;
  }
  hogd::info("starting indexer!");

  std::optional<std::string> tx_hash;
  int opt = 0;
  // NOLINTBEGIN(misc-include-cleaner)
  while ((opt = getopt(argc, argv, "t:")) != -1) {
    switch (opt) {
    case 't':
      tx_hash = optarg;
      break;
    default:
      hogd::print_usage();
      return EXIT_FAILURE;
    }
  }
  if (optind >= argc) {
    hogd::print_usage();
    return EXIT_FAILURE;
  }

  const std::string indexer_server_addr = argv[optind];
  // NOLINTEND(misc-include-cleaner)
  return hogd::block_explorer_init(indexer_server_addr, tx_hash);
};

// NOLINTEND(bugprone-exception-escape)
