#include <algorithm>
#include <cstdlib>
#include <limits>
#include <map>
#include <print>

#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h"
#include "hogd/init.h"
#include "hogd/loadgen/erc20_experiment.h"
#include "loadgen.hh"
#include "transaction.hh"
#include "types.hh"
#include "x23/serialize.h"
#include "x23/serialize_cxs.h"

namespace hogd {
namespace {

void
print_tx_size_stats()
{
  // Create an ERC20 experiment with a moderate number of accounts
  const uint32_t num_accounts = 100;
  const uint64_t num_txs = 1000;
  const BlockHeight expiry_height = 100;

  std::println("Creating ERC20 experiment with {} accounts...", num_accounts);

  auto exp = ERC20ExperimentState(ERC20PaymentsConfig{
      .num_addresses = num_accounts,
  });

  std::println("Generating experiment...");
  auto init_result = exp.generate_experiment()();
  if (!init_result.has_value()) {
    std::println(stderr, "Failed to generate ERC20 experiment");
    return;
  }

  std::println("Extending experiment to generate {} transactions...", num_txs);
  auto result = exp.extend_experiment(num_txs, expiry_height)();
  if (!result.has_value()) {
    std::println(stderr, "Failed to extend ERC20 experiment");
    return;
  }
  SignedTransactionBatch work_txs;
  parallel_batch_to_t(result.value().work_txs, work_txs, kNoBatchSizeLimit)();

  std::println("Generated {} transactions\n", work_txs.size());

  // Collect TXS size statistics
  std::map<size_t, uint64_t> txs_size_counts;
  size_t txs_min_size = std::numeric_limits<size_t>::max();
  size_t txs_max_size = 0;
  size_t txs_total_size = 0;

  // Collect CXS size statistics
  std::map<size_t, uint64_t> cxs_size_counts;
  size_t cxs_min_size = std::numeric_limits<size_t>::max();
  size_t cxs_max_size = 0;
  size_t cxs_total_size = 0;

  for (const auto &tx : work_txs) {
    // TXS (default XDR) serialization
    size_t txs_size = x23::xdr_size(tx);
    txs_size_counts[txs_size]++;
    txs_min_size = std::min<size_t>(txs_min_size, txs_size);
    txs_max_size = std::max<size_t>(txs_max_size, txs_size);
    txs_total_size += txs_size;

    // CXS serialization
    auto cxs_buf = x23::serialize_cxs(tx);
    if (!cxs_buf) {
      std::println(stderr, "CXS serialization failed: {}", cxs_buf.error());
      return;
    }
    size_t cxs_size = cxs_buf->size();
    cxs_size_counts[cxs_size]++;
    cxs_min_size = std::min<size_t>(cxs_min_size, cxs_size);
    cxs_max_size = std::max<size_t>(cxs_max_size, cxs_size);
    cxs_total_size += cxs_size;
  }

  size_t num_tx = work_txs.size();

  // Print TXS statistics
  std::println("TXS (XDR) Serialization Size Statistics:");
  std::println("==========================================");
  std::println("Total transactions: {}", num_tx);
  std::println("Min size: {} bytes", txs_min_size);
  std::println("Max size: {} bytes", txs_max_size);
  std::println("Average size: {} bytes", txs_total_size / num_tx);
  std::println("");

  if (txs_size_counts.size() == 1) {
    std::println("All transactions have the same size: {} bytes ✓",
                 txs_min_size);
  }
  else {
    std::println("Size distribution:");
    for (const auto &[size, count] : txs_size_counts) {
      std::println("  {} bytes: {} transactions", size, count);
    }
  }

  std::println("\nCXS Serialization Size Statistics:");
  std::println("==========================================");
  std::println("Total transactions: {}", num_tx);
  std::println("Min size: {} bytes", cxs_min_size);
  std::println("Max size: {} bytes", cxs_max_size);
  std::println("Average size: {} bytes", cxs_total_size / num_tx);
  std::println("");

  if (cxs_size_counts.size() == 1) {
    std::println("All transactions have the same size: {} bytes ✓",
                 cxs_min_size);
  }
  else {
    std::println("Size distribution:");
    for (const auto &[size, count] : cxs_size_counts) {
      std::println("  {} bytes: {} transactions", size, count);
    }
  }

  std::println("\nComparison:");
  std::println("==========================================");
  std::println("CXS is {:.1f}% the size of TXS",
               (100.0 * cxs_total_size) / txs_total_size);
  std::println("Space savings: {} bytes ({:.1f}%)",
               txs_total_size - cxs_total_size,
               100.0 * (txs_total_size - cxs_total_size) / txs_total_size);
}

} // namespace
} // namespace hogd

// NOLINTBEGIN(bugprone-exception-escape)
int
main(int argc, char *argv[])
{
  auto ret = hogd::hogd_init(argc, argv);
  if (ret)
    return EXIT_FAILURE;

  hogd::print_tx_size_stats();
  return EXIT_SUCCESS;
}
// NOLINTEND(bugprone-exception-escape)
