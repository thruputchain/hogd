#include <cstdlib>
#include <format>
#include <future>
#include <optional>
#include <print>
#include <string>
#include <unistd.h>

#include "hogd/base/log.h"
#include "hogd/base/network.h"
#include "hogd/base/rpc_client.h"
#include "hogd/base/types.h"
#include "hogd/block_explorer/shared.h"
#include "hogd/container/merkle/hex_array.h"
#include "hogd/init.h"
#include "hogd/loadgen/loadgen_server.h"
#include "hogd/version.h"
#include "indexer.hh"

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
      client.indexer_block_explorer_intro(
          /*protocol=*/version::kHogdProtocol())();
  if (intro_result != IndexerBlockExplorerIntroOutcome::IntroSuccess) {
    info("block explorer failed to connect to indexer (error: {})",
         intro_result);
    return EXIT_FAILURE;
  }

  SHA256Hash tx_hash = HexArray(*tx_hash_raw).to_sha256();
  TxInfoResult tx_info_res = client.get_transaction_info(tx_hash)();
  if (tx_info_res.outcome() == TxInfoOutcome::TxInfoFound) {
    const TxInfo &tx_info = tx_info_res.info();
    info("tx info:");
    info("hash = {}", *tx_hash_raw);
    info("block_height = {}", tx_info.height);
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
  return hogd::block_explorer_init(indexer_server_addr, tx_hash);
};

// NOLINTEND(bugprone-exception-escape)
