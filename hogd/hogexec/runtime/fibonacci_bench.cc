#include <benchmark/benchmark.h>

#include <cstddef>
#include <cstdlib>
#include <stdexcept>
#include <vector>

#include "hogd/base/status.h"
#include "hogd/hogexec/runtime/erc20_test_utils.h"
#include "hogd/hogexec/runtime/runtime_test_framework.h"
#include "hogd/init.h"

#include "types.hh"

namespace hogd {

namespace {
void
bm_fibonacci(benchmark::State &state)
{
  test::RuntimeTestFramework runtime_framework;

  runtime_framework.initialize();

  auto [h, c] = test::load_built_rs_contract("fibonacci_bench.wasm");

  ContractAddress addr = ContractAddress{}; // actual address doesn't matter

  runtime_framework.contract_map_->add_contract(h, c);
  runtime_framework.contract_map_->deploy_contract(addr, h);
  runtime_framework.contract_map_->commit_round();

  SignedTransaction stx;
  stx.tx.invocation.invoked_address = addr;
  stx.tx.invocation.method = test::get_sdk_name("fibonacci_bench_root");
  stx.tx.invocation.gas_limit = UINT64_MAX;

  uint32_t fibonacci_depth = state.range(0);

  for (auto _ : state) {
    stx.tx.invocation.calldata = test::make_calldata(fibonacci_depth);

    SystemCallHandler handler(stx, *runtime_framework.snapshot_cache_);
    if (!handler.root_invoke().result.has_value()) {
      throw std::runtime_error("fibonacci tx failed!");
    }
  }
}

BENCHMARK(bm_fibonacci)->RangeMultiplier(4)->Range(4, 2 << 28); // NOLINT

} // namespace

} // namespace hogd

int
main(int argc, char *argv[])
{
  using hogd::GStatus;
  GStatus<void> ret = hogd::crypto_init();
  if (!ret)
    return EXIT_FAILURE;
  benchmark::Initialize(&argc, argv);
  benchmark::RunSpecifiedBenchmarks();
}
