#include <benchmark/benchmark.h>

#include <memory>

#include "account.hh"
#include "db_sched.hh"
#include "discovery.hh"
#include "exec_sched.hh"
#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "hogd/mempool/authentication.h"
#include "hogd/mempool/rpc_from_coord.h"
#include "hogd/mempool/test/infra.h"
#include "hogd/mempool/test/txs.h"
#include "nomination.hh"
#include "sched_coord.hh"
#include "transaction.hh"
#include "types.hh"

namespace hogd {

namespace {

constexpr size_t kNumTxs = 1000;

SignedTransactionBatch
generate_signed_txs(fib_client_t &fib_client, MempoolTestInfra &server,
                    size_t num_txs)
{
  SignedTransactionBatch unsigned_txs =
      generate_unsigned_txs(fib_client, num_txs);
  AccountAuthData auth_data = get_default_account();
  auth_data.nonce_config.required_weight = 1;
  set_nonce_key(auth_data, /*idx=*/0);
  add_gas_config(auth_data, /*gas=*/5'000'000'000, /*keys=*/{0},
                 /*required_weight=*/1);
  const Ed25519SecretKey private_key =
      create_key_pair(auth_data, /*idx=*/0, /*weight=*/1);
  // TODO(jhumphri): Add this to each of the tests and more extensively verify
  // the output of nominations, etc.
  server.set_account_auth_data(MempoolTestInfra::kAccountAddresses[0],
                               auth_data);

  SignedTransactionBatch txs;
  for (const SignedTransaction &unsigned_tx : unsigned_txs) {
    txs.push_back(hogd::Authentication::sign(unsigned_tx.tx,
                                             /*signature_weight=*/1,
                                             {{0, private_key}},
                                             /*auxiliary_private_keys=*/{}));
  }
  return txs;
}

void
submit_unvetted(benchmark::State &state)
{
  for (auto _ : state) {
    state.PauseTiming();
    fib_client_t fib_client(MempoolTestInfra::kAccountAddresses[0]);
    MempoolTestInfra server;
    const SignedTransactionBatch txs =
        generate_signed_txs(fib_client, server, kNumTxs);
    state.ResumeTiming();

    server.mempool_coord_rpc().schedulercoord_submit_unvetted(txs)();
  }
}
BENCHMARK(submit_unvetted)->Unit(benchmark::kMillisecond);

void
submit_vetted(benchmark::State &state)
{
  for (auto _ : state) {
    state.PauseTiming();
    fib_client_t fib_client(MempoolTestInfra::kAccountAddresses[0]);
    MempoolTestInfra server;
    const SignedTransactionBatch txs =
        generate_signed_txs(fib_client, server, kNumTxs);
    state.ResumeTiming();

    ValidatorID validator_id{};
    server.mempool_coord_rpc().schedulercoord_submit_vetted(validator_id,
                                                            txs)();
  }
}
BENCHMARK(submit_vetted)->Unit(benchmark::kMicrosecond);

void
nominate(benchmark::State &state)
{
  for (auto _ : state) {
    state.PauseTiming();
    fib_client_t fib_client(MempoolTestInfra::kAccountAddresses[0]);
    MempoolTestInfra server;
    const SignedTransactionBatch txs =
        generate_signed_txs(fib_client, server, kNumTxs);
    // Submit unvetted transactions so there's something to nominate
    server.mempool_coord_rpc().schedulercoord_submit_unvetted(txs)();
    DbTip db_tip{.height = 0, .hash = {}, .id = 0};
    state.ResumeTiming();

    auto res_nominate =
        server.mempool_coord_rpc().schedulercoord_nominate(db_tip)();
    CHECK_EQ(res_nominate.outcome(),
             SchedulerNominationOutcome::SchedulerNominationOK);
  }
}
BENCHMARK(nominate)->Unit(benchmark::kMillisecond);

void
validate(benchmark::State &state)
{
  for (auto _ : state) {
    state.PauseTiming();
    fib_client_t fib_client(MempoolTestInfra::kAccountAddresses[0]);
    MempoolTestInfra server;
    const SignedTransactionBatch txs =
        generate_signed_txs(fib_client, server, kNumTxs);

    server.mempool_coord_rpc().schedulercoord_submit_unvetted(txs)();

    DbTip db_tip{.height = 0, .hash = {}, .id = 0};
    auto res_nominate =
        server.mempool_coord_rpc().schedulercoord_nominate(db_tip)();
    CHECK_EQ(res_nominate.outcome(),
             SchedulerNominationOutcome::SchedulerNominationOK);
    state.ResumeTiming();

    ValidatorID validator_id{};
    server.mempool_coord_rpc().schedulercoord_submit_vetted(validator_id,
                                                            txs)();
    auto res_validate = server.mempool_coord_rpc().schedulercoord_validate(
        db_tip, res_nominate.scheduler_nomination().nomination)();
    CHECK_EQ(res_validate.outcome(),
             SchedulerValidationResult::SchedulerValidationOK);
  }
}
BENCHMARK(validate)->Unit(benchmark::kMillisecond);

struct Init {
  Init()
  {
    // Initialize logging. This is needed for the CHECK macros.
    log_init(std::make_unique<ConsoleLogger>());
  }
};

// This runs [Init]'s constructor at program startup to do initialization.
Init init;

} // namespace

} // namespace hogd

BENCHMARK_MAIN();
