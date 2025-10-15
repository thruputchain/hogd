#include <benchmark/benchmark.h>

#include <memory>
#include <print>

#include "account.hh"
#include "auth_claim.hh"
#include "db_sched.hh"
#include "discovery.hh"
#include "exec_sched.hh"
#include "hogd/base/coropoll.h"
#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "hogd/loadgen/loadgen_server.h"
#include "hogd/mempool/authentication.h"
#include "hogd/mempool/rpc_from_coord.h"
#include "hogd/mempool/test/infra.h"
#include "hogd/mempool/test/txs.h"
#include "loadgen.hh"
#include "nomination.hh"
#include "sched_coord.hh"
#include "transaction.hh"
#include "types.hh"
#include "x23/cleanup.h"
#include "x23/rpc.h"
#include "x23/sockio.h"
#include "x23/vec.h"

namespace hogd {

namespace {

constexpr size_t kNumTxs = 1000;
constexpr size_t kNumLoadgenTxs = 10'000;
constexpr uint32_t kNumAccounts = 100;

// Helper class to manage a loadgen server and client
class LoadgenHelper {
public:
  using loadgen_client_t =
      LoadgenInterfaceV1::client<x23::RpcStream<x23::RpcLogger>::ClientRef>;

  LoadgenHelper()
  {
    // Start loadgen server with auto-assigned port
    uint16_t port = server_.start_server(/*port=*/0);
    info("Started loadgen server on port {}", port);

    // Connect to the loadgen server
    x23::UniqueFd fd =
        x23::tcpconnect(*x23::HostPort{"127.0.0.1", port}.to_addr());
    CHECK(fd.operator bool());

    auto stream = x23::rpc_stream_logged(default_coropoll(), fd.release());
    client_ = std::make_unique<loadgen_client_t>(
        stream->client(LoadgenInterfaceV1{}));
  }

  // TODO(jhumphri): Add flag to infra.h to disable existing setup for tests. We
  // do not want that setup when using loadgen.
  void init_experiment(MempoolTestInfra &infra)
  {
    // Initialize the experiment.
    ExperimentConfig config(ExperimentType::kExperimentTypeERC20Payments);
    config.erc20_payments().num_accounts = kNumAccounts;

    LoadgenResult result = client_->generate_experiment(config)();
    CHECK_EQ(result.error(), LoadgenRPCError::kLoadgenRPCErrorOK);
    context_id_ = result.success().context_id;

    // Pass the genesis block to the db.
    CHECK_EQ(infra.mempool_coord_rpc().send_genesis_block(
                 result.success().genesis_block)(),
             DbSchedResult::DbSchedResultOK);

    // We need to nominate and commit the initialization transactions. So first,
    // we start by adding them to the mempool with
    // [schedulercoord_submit_vetted()].
    const x23::Vec<InitializationBlock> &init_txs =
        result.success().initialization_txs;
    constexpr ValidatorID kValidatorId{};
    for (const InitializationBlock &init_tx : init_txs) {
      infra.mempool_coord_rpc().schedulercoord_submit_vetted(kValidatorId,
                                                             init_tx)();

      // Now nominate and commit the initialization transactions with
      // [schedulercoord_nominate()].
      size_t num_nominated_txs = 0;
      SchedulerNomination nomination{};
      while (num_nominated_txs < init_tx.size()) {
        DbTip db_tip{.height = nomination.nomination.block_height,
                     .hash = nomination.nomination.block_hash,
                     .id = nomination.block_id};
        auto res_nominate =
            infra.mempool_coord_rpc().schedulercoord_nominate(db_tip)();
        infra.mempool_coord_rpc().dump_stats();
        CHECK_EQ(res_nominate.outcome(),
                 SchedulerNominationOutcome::SchedulerNominationOK);
        nomination = res_nominate.scheduler_nomination();

        num_nominated_txs += nomination.nomination.transaction_hashes.size();
      }
      CHECK_EQ(num_nominated_txs, init_tx.size());
    }

    info("Initialized loadgen experiment with context_id {}", context_id_);
  }

  SignedTransactionBatch get_transactions(size_t num_txs)
  {
    LoadgenExtensionResult result =
        client_->extend_experiment(context_id_, num_txs, 1'000'000'000)();
    CHECK_EQ(result.error(), LoadgenRPCError::kLoadgenRPCErrorOK);
    return result.success().work_txs;
  }

  ~LoadgenHelper() { server_.shut_down(); }

private:
  LoadgenServer server_;
  std::unique_ptr<loadgen_client_t> client_;
  LoadgenContextID context_id_;
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

    ValidatorID validator_id{};
    server.mempool_coord_rpc().schedulercoord_submit_vetted(validator_id,
                                                            txs)();
    state.ResumeTiming();

    auto res_validate = server.mempool_coord_rpc().schedulercoord_validate(
        db_tip, res_nominate.scheduler_nomination().nomination)();
    CHECK_EQ(res_validate.outcome(),
             SchedulerValidationResult::SchedulerValidationOK);
  }
}
BENCHMARK(validate)->Unit(benchmark::kMillisecond)->Iterations(3);

// Loadgen-based benchmarks.
void
loadgen_submit_unvetted(benchmark::State &state)
{
  MempoolTestInfra server;
  LoadgenHelper loadgen;
  loadgen.init_experiment(server);

  for (auto _ : state) {
    state.PauseTiming();
    const SignedTransactionBatch txs = loadgen.get_transactions(kNumLoadgenTxs);
    state.ResumeTiming();

    server.mempool_coord_rpc().schedulercoord_submit_unvetted(txs)();
  }
}
BENCHMARK(loadgen_submit_unvetted)->Unit(benchmark::kMillisecond);

void
loadgen_submit_vetted(benchmark::State &state)
{
  MempoolTestInfra server;
  LoadgenHelper loadgen;
  loadgen.init_experiment(server);

  for (auto _ : state) {
    state.PauseTiming();
    const SignedTransactionBatch txs = loadgen.get_transactions(kNumLoadgenTxs);
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
  MempoolTestInfra server;
  LoadgenHelper loadgen;
  loadgen.init_experiment(server);

  for (auto _ : state) {
    state.PauseTiming();
    const SignedTransactionBatch txs = loadgen.get_transactions(kNumLoadgenTxs);
    // Submit unvetted transactions so there's something to nominate
    ValidatorID validator_id{};
    server.mempool_coord_rpc().schedulercoord_submit_vetted(validator_id,
                                                            txs)();
    DbTip db_tip{.height = 0, .hash = {}, .id = 0};
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
  MempoolTestInfra server;
  LoadgenHelper loadgen;
  loadgen.init_experiment(server);

  for (auto _ : state) {
    state.PauseTiming();
    const SignedTransactionBatch txs = loadgen.get_transactions(kNumLoadgenTxs);

    ValidatorID validator_id{};
    server.mempool_coord_rpc().schedulercoord_submit_vetted(validator_id,
                                                            txs)();

    DbTip db_tip{.height = 0, .hash = {}, .id = 0};
    auto res_nominate =
        server.mempool_coord_rpc().schedulercoord_nominate(db_tip)();
    CHECK_EQ(res_nominate.outcome(),
             SchedulerNominationOutcome::SchedulerNominationOK);

    server.mempool_coord_rpc().schedulercoord_submit_vetted(validator_id,
                                                            txs)();
    state.ResumeTiming();

    auto res_validate = server.mempool_coord_rpc().schedulercoord_validate(
        db_tip, res_nominate.scheduler_nomination().nomination)();
    CHECK_EQ(res_validate.outcome(),
             SchedulerValidationResult::SchedulerValidationOK);
  }
}
BENCHMARK(loadgen_validate)->Unit(benchmark::kMillisecond)->Iterations(3);

void
loadgen_nominate_throughput(benchmark::State &state)
{
  constexpr size_t kTotalTxsToRun = 100000;

  MempoolTestInfra server;
  LoadgenHelper loadgen;
  loadgen.init_experiment(server);

  for (auto _ : state) {
    size_t txs_so_far = 0;

    while (txs_so_far < kTotalTxsToRun) {
      std::println("txs_so_far {}, kTotalTxsToRun {}", txs_so_far,
                   kTotalTxsToRun);
      // TODO(jhumphri): Get txs from loadgen in a separate thread.
      const SignedTransactionBatch txs =
          loadgen.get_transactions(kNumLoadgenTxs);

      ValidatorID validator_id{};
      server.mempool_coord_rpc().schedulercoord_submit_vetted(validator_id,
                                                              txs)();

      DbTip db_tip{.height = 0, .hash = {}, .id = 0};
      auto res_nominate =
          server.mempool_coord_rpc().schedulercoord_nominate(db_tip)();
      CHECK_EQ(res_nominate.outcome(),
               SchedulerNominationOutcome::SchedulerNominationOK);

      txs_so_far += res_nominate.scheduler_nomination()
                        .nomination.transaction_hashes.size();
    }
  }
}
BENCHMARK(loadgen_nominate_throughput)->Unit(benchmark::kSecond)->Iterations(1);

void
loadgen_validate_throughput(benchmark::State &state)
{
  constexpr size_t kTotalTxsToRun = 100000;

  MempoolTestInfra server;
  LoadgenHelper loadgen;
  loadgen.init_experiment(server);

  const SignedTransactionBatch txs = loadgen.get_transactions(kNumLoadgenTxs);

  ValidatorID validator_id{};
  server.mempool_coord_rpc().schedulercoord_submit_vetted(validator_id, txs)();

  DbTip db_tip{.height = 0, .hash = {}, .id = 0};
  auto res_nominate =
      server.mempool_coord_rpc().schedulercoord_nominate(db_tip)();
  CHECK_EQ(res_nominate.outcome(),
           SchedulerNominationOutcome::SchedulerNominationOK);

  for (auto _ : state) {
    size_t txs_so_far = 0;

    while (txs_so_far < kTotalTxsToRun) {
      std::println("txs_so_far {}, kTotalTxsToRun {}", txs_so_far,
                   kTotalTxsToRun);
      ValidatorID validator_id{};
      server.mempool_coord_rpc().schedulercoord_submit_vetted(validator_id,
                                                              txs)();

      auto res_validate = server.mempool_coord_rpc().schedulercoord_validate(
          db_tip, res_nominate.scheduler_nomination().nomination)();
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
