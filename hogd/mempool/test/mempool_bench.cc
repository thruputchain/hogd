#include <benchmark/benchmark.h>

#include <format>
#include <memory>
#include <print>
#include <string>

#include "account.hh"
#include "auth_claim.hh"
#include "db_sched.hh"
#include "discovery.hh"
#include "exec_sched.hh"
#include "hogd/base/log.h"
#include "hogd/base/rpc_server.h"
#include "hogd/base/types.h"
#include "hogd/loadgen/loadgen_server.h"
#include "hogd/mempool/authentication.h"
#include "hogd/mempool/rpc_from_coord.h"
#include "hogd/mempool/test/infra.h"
#include "hogd/mempool/test/loadgen.h"
#include "hogd/mempool/test/txs.h"
#include "nomination.hh"
#include "sched_coord.hh"
#include "transaction.hh"
#include "types.hh"
#include "x23/vec.h"

namespace hogd {

namespace {

constexpr size_t kNumTxs = 1000;
constexpr size_t kNumLoadgenTxs = 100'000;
constexpr uint32_t kNumAccounts = 10'000;

// Helper class to manage a loadgen server and client.
class LoadgenHelper {
public:
  LoadgenHelper()
  {
    // Start loadgen server with auto-assigned port
    uint16_t port = server_.start_server(/*port=*/0);
    info("Started loadgen server on port {}", port);

    const std::string loadgen_server_address =
        std::format("{}:{}", MempoolTestInfra::kAddress, port);
    client_ = std::make_unique<LoadgenClient>(loadgen_server_address);
  }

  DbTip init_experiment(MempoolTestInfra &infra)
  {
    return client_->init_experiment(infra.mempool_coord_rpc(), kNumAccounts);
  }

  x23::Async<SignedTransactionBatch> get_transactions(size_t num_txs)
  {
    return client_->get_transactions(num_txs);
  }

  ~LoadgenHelper() { server_.shut_down(); }

private:
  RpcServer<LoadgenRpc> server_;
  std::unique_ptr<LoadgenClient> client_;
};

constexpr AuthClaim
make_self_sign_claim(uint32_t key_id)
{
  AuthClaim claim(AuthClaimType::kInvokedAddressSignature);
  claim.invoked_address_signature_id() = key_id;
  return claim;
}

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
  for (SignedTransaction &unsigned_tx : unsigned_txs) {
    unsigned_tx.tx.auth_claims.push_back(make_self_sign_claim(0));
    txs.push_back(
        hogd::Authentication::sign_claims(unsigned_tx.tx, {{0, private_key}},
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
BENCHMARK(nominate)->Unit(benchmark::kMillisecond)->Iterations(3);

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

    // We reuse the [Nomination] struct and make some adjustments for the
    // validation.
    Nomination nomination = res_nominate.scheduler_nomination().nomination;
    db_tip.height++;
    db_tip.hash = nomination.block_hash;
    db_tip.id++;

    nomination.block_height++;
    nomination.prev_block_hash = nomination.block_hash;
    nomination.block_hash = MempoolCoordinatorRpc::get_block_hash(
        nomination.transaction_hashes,
        /*prev_block_hash=*/nomination.prev_block_hash);

    ValidatorID validator_id{};
    server.mempool_coord_rpc().schedulercoord_submit_vetted(validator_id,
                                                            txs)();
    state.ResumeTiming();

    auto res_validate =
        server.mempool_coord_rpc().schedulercoord_validate(db_tip,
                                                           nomination)();
    CHECK_EQ(res_validate.outcome(),
             SchedulerValidationResult::SchedulerValidationOK);
  }
}
BENCHMARK(validate)->Unit(benchmark::kMillisecond)->Iterations(3);

// Loadgen-based benchmarks.
void
loadgen_submit_unvetted(benchmark::State &state)
{
  MempoolTestInfra server(/*skip_genesis_block=*/true);
  LoadgenHelper loadgen;
  loadgen.init_experiment(server);

  for (auto _ : state) {
    state.PauseTiming();
    const SignedTransactionBatch txs =
        loadgen.get_transactions(MempoolCoordinatorRpc::kMaxBlockSize)();
    state.ResumeTiming();

    server.mempool_coord_rpc().schedulercoord_submit_unvetted(txs)();
  }
}
BENCHMARK(loadgen_submit_unvetted)->Unit(benchmark::kMillisecond);

void
loadgen_submit_vetted(benchmark::State &state)
{
  MempoolTestInfra server(/*skip_genesis_block=*/true);
  LoadgenHelper loadgen;
  loadgen.init_experiment(server);

  for (auto _ : state) {
    state.PauseTiming();
    const SignedTransactionBatch txs =
        loadgen.get_transactions(MempoolCoordinatorRpc::kMaxBlockSize)();
    state.ResumeTiming();

    ValidatorID validator_id{};
    server.mempool_coord_rpc().schedulercoord_submit_vetted(validator_id,
                                                            txs)();
  }
}
BENCHMARK(loadgen_submit_vetted)->Unit(benchmark::kMicrosecond);

void
loadgen_nominate(benchmark::State &state)
{
  MempoolTestInfra server(/*skip_genesis_block=*/true);
  LoadgenHelper loadgen;
  DbTip db_tip = loadgen.init_experiment(server);

  for (auto _ : state) {
    state.PauseTiming();
    const SignedTransactionBatch txs =
        loadgen.get_transactions(MempoolCoordinatorRpc::kMaxBlockSize)();

    ValidatorID validator_id{};
    server.mempool_coord_rpc().schedulercoord_submit_vetted(validator_id,
                                                            txs)();
    state.ResumeTiming();

    auto res_nominate =
        server.mempool_coord_rpc().schedulercoord_nominate(db_tip)();
    CHECK_EQ(res_nominate.outcome(),
             SchedulerNominationOutcome::SchedulerNominationOK);
  }
}
BENCHMARK(loadgen_nominate)->Unit(benchmark::kMillisecond);

void
loadgen_validate(benchmark::State &state)
{
  MempoolTestInfra server(/*skip_genesis_block=*/true);
  LoadgenHelper loadgen;
  DbTip db_tip = loadgen.init_experiment(server);

  for (auto _ : state) {
    state.PauseTiming();
    const SignedTransactionBatch txs =
        loadgen.get_transactions(MempoolCoordinatorRpc::kMaxBlockSize)();

    ValidatorID validator_id{};
    server.mempool_coord_rpc().schedulercoord_submit_vetted(validator_id,
                                                            txs)();

    auto res_nominate =
        server.mempool_coord_rpc().schedulercoord_nominate(db_tip)();
    CHECK_EQ(res_nominate.outcome(),
             SchedulerNominationOutcome::SchedulerNominationOK);

    // We reuse the [Nomination] struct and make some adjustments for the
    // validation.
    Nomination nomination = res_nominate.scheduler_nomination().nomination;
    db_tip.height++;
    db_tip.hash = nomination.block_hash;
    db_tip.id++;

    nomination.block_height++;
    nomination.prev_block_hash = nomination.block_hash;
    nomination.block_hash = MempoolCoordinatorRpc::get_block_hash(
        nomination.transaction_hashes,
        /*prev_block_hash=*/nomination.prev_block_hash);

    server.mempool_coord_rpc().schedulercoord_submit_vetted(validator_id,
                                                            txs)();
    state.ResumeTiming();

    auto res_validate =
        server.mempool_coord_rpc().schedulercoord_validate(db_tip,
                                                           nomination)();
    CHECK_EQ(res_validate.outcome(),
             SchedulerValidationResult::SchedulerValidationOK);
  }
}
BENCHMARK(loadgen_validate)->Unit(benchmark::kMillisecond)->Iterations(3);

void
loadgen_nominate_throughput(benchmark::State &state)
{
  constexpr size_t kTotalTxsToRun = 100'000;

  MempoolTestInfra server(/*skip_genesis_block=*/true);
  LoadgenHelper loadgen;
  DbTip db_tip = loadgen.init_experiment(server);
  const SignedTransactionBatch txs = loadgen.get_transactions(kNumLoadgenTxs)();

  for (auto _ : state) {
    state.PauseTiming();

    ValidatorID validator_id{};
    server.mempool_coord_rpc().schedulercoord_submit_vetted(validator_id,
                                                            txs)();
    state.ResumeTiming();

    size_t txs_so_far = 0;
    while (txs_so_far < kTotalTxsToRun) {
      std::println("txs_so_far {}, kTotalTxsToRun {}", txs_so_far,
                   kTotalTxsToRun);

      auto res_nominate =
          server.mempool_coord_rpc().schedulercoord_nominate(db_tip)();
      CHECK_EQ(res_nominate.outcome(),
               SchedulerNominationOutcome::SchedulerNominationOK);

      const SchedulerNomination nomination =
          res_nominate.scheduler_nomination();
      db_tip = {.height = nomination.nomination.block_height,
                .hash = nomination.nomination.block_hash,
                .id = nomination.block_id};
      txs_so_far += nomination.nomination.transaction_hashes.size();
    }
  }
}
BENCHMARK(loadgen_nominate_throughput)->Unit(benchmark::kSecond)->Iterations(1);

void
loadgen_validate_throughput(benchmark::State &state)
{
  constexpr size_t kTotalTxsToRun = 100'000;

  MempoolTestInfra server(/*skip_genesis_block=*/true);
  LoadgenHelper loadgen;
  DbTip db_tip = loadgen.init_experiment(server);
  const SignedTransactionBatch txs = loadgen.get_transactions(kTotalTxsToRun)();

  for (auto _ : state) {
    state.PauseTiming();

    ValidatorID validator_id{};
    server.mempool_coord_rpc().schedulercoord_submit_vetted(validator_id,
                                                            txs)();

    auto res_nominate =
        server.mempool_coord_rpc().schedulercoord_nominate(db_tip)();
    CHECK_EQ(res_nominate.outcome(),
             SchedulerNominationOutcome::SchedulerNominationOK);

    state.ResumeTiming();

    size_t txs_so_far = 0;
    Nomination nomination = res_nominate.scheduler_nomination().nomination;
    while (txs_so_far < kTotalTxsToRun) {
      std::println("txs_so_far {}, kTotalTxsToRun {}", txs_so_far,
                   kTotalTxsToRun);

      // We reuse the [Nomination] struct and make some adjustments for the
      // validation.
      db_tip.height++;
      db_tip.hash = nomination.block_hash;
      db_tip.id++;

      nomination.block_height++;
      nomination.prev_block_hash = nomination.block_hash;
      nomination.block_hash = MempoolCoordinatorRpc::get_block_hash(
          nomination.transaction_hashes,
          /*prev_block_hash=*/nomination.prev_block_hash);

      ValidatorID validator_id{};
      server.mempool_coord_rpc().schedulercoord_submit_vetted(validator_id,
                                                              txs)();

      auto res_validate =
          server.mempool_coord_rpc().schedulercoord_validate(db_tip,
                                                             nomination)();
      CHECK_EQ(res_validate.outcome(),
               SchedulerValidationResult::SchedulerValidationOK);
      txs_so_far += res_nominate.scheduler_nomination()
                        .nomination.transaction_hashes.size();
    }
  }
}
BENCHMARK(loadgen_validate_throughput)->Unit(benchmark::kSecond)->Iterations(1);

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
