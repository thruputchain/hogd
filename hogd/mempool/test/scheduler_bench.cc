#include <benchmark/benchmark.h>

#include <array>
#include <cstddef>
#include <cstdlib>
#include <memory>

#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h"
#include "hogd/init.h"
#include "hogd/mempool/scheduler.h"
#include "hogd/mempool/test/infra.h"
#include "transaction.hh"
#include "x23/vec.h"

namespace hogd {

namespace {

SignedTransaction
make_tx_for_account(const ContractAddress &account, uint64_t nonce)
{
  SignedTransaction tx{};
  tx.tx.invocation.invoked_address = account;
  tx.tx.uniquifier.nonce = nonce;
  tx.tx.uniquifier.block_expiry_height = 100;
  return tx;
}

void
bm_scheduler_read_average_hog_ages_txs_hot_cache(benchmark::State &state)
{
  MempoolTestInfra server(/*skip_genesis_block=*/true);
  std::shared_ptr<Scheduler> scheduler = server.mempool_coord_rpc().scheduler();

  x23::XdrVec<SignedTransaction> txs;
  txs.reserve(state.range(0));
  for (int i = 0; i < state.range(0); ++i) {
    ContractAddress account{};
    account[0] = static_cast<uint8_t>(i & 0xff);
    account[1] = static_cast<uint8_t>((i >> 8) & 0xff);
    account[2] = static_cast<uint8_t>((i >> 16) & 0xff);
    server.set_hogs(account, kHogAgeGroupToSchedule, /*age=*/0,
                    /*num_hogs=*/1);
    txs.push_back(make_tx_for_account(account, i));
  }

  scheduler->read_average_hog_ages(txs)();

  for (auto _ : state) {
    scheduler->read_average_hog_ages(txs)();
  }
}

BENCHMARK(bm_scheduler_read_average_hog_ages_txs_hot_cache)
    ->Arg(1000)
    ->Arg(10000)
    ->UseRealTime();

} // namespace

} // namespace hogd

int
main(int argc, char *argv[])
{
  auto ret = hogd::hogd_init(argc, argv);
  if (ret != 0) {
    return EXIT_FAILURE;
  }
  benchmark::Initialize(&argc, argv);
  benchmark::RunSpecifiedBenchmarks();
}
