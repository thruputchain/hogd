#include <benchmark/benchmark.h>

#include <memory>

#include "account.hh"
#include "db_sched.hh"
#include "discovery.hh"
#include "exec_sched.hh"
#include "hogd/base/log.h"
#include "hogd/mempool/authentication.h"
#include "hogd/mempool/rpc_from_coord.h"
#include "hogd/mempool/test/infra.h"
#include "nomination.hh"
#include "sched_coord.hh"
#include "transaction.hh"
#include "types.hh"

namespace hogd {

namespace {

void
stress(benchmark::State &state)
{
  fib_client_t fib_client(MempoolTestInfra::kAccountAddresses[0]);
  const SignedTransactionBatch txs = generate_txs(fib_client, kNumTxs);

  MempoolTestInfra server;

  ValidatorID validator_id{};
  server.mempool_coord_rpc().schedulercoord_submit_vetted(validator_id, txs)();

  DbTip db_tip{.height = 0, .hash = {}, .id = 0};
  auto res_nominate =
      server.mempool_coord_rpc().schedulercoord_nominate(db_tip)();
  CHECK_EQ(res_nominate.outcome(),
           SchedulerNominationOutcome::SchedulerNominationOK);

  server.mempool_coord_rpc().schedulercoord_submit_vetted(validator_id, txs)();
  auto res_validate = server.mempool_coord_rpc().schedulercoord_validate(
      db_tip, res_nominate.scheduler_nomination().nomination)();
  CHECK_EQ(res_validate.outcome(),
           SchedulerValidationResult::SchedulerValidationOK);
}
BENCHMARK(stress);

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
