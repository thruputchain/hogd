#include <benchmark/benchmark.h>

#include <cstddef>
#include <cstdlib>
#include <stdexcept>
#include <vector>

#include "hogd/base/status.h"
#include "hogd/hogexec/runtime/erc20_test_utils.h"
#include "hogd/hogexec/runtime/runtime_test_framework.h"
#include "hogd/hogexec/runtime/wasmi_runtime.h"
#include "hogd/init.h"

#include "types.hh"

namespace hogd {

namespace {
void
bm_erc20_2wallets(benchmark::State &state)
{
  test::RuntimeTestFramework runtime_framework;

  runtime_framework.initialize();
  runtime_framework.deploy_genesis_block();

  ContractAddress erc20_address;
  ContractAddress manager_address;

  std::vector<ContractAddress> wallets;

  // Create an ERC20 contract
  auto r = test::erc20::deploy_erc20_noinit(runtime_framework);
  if (!r) {
    throw std::runtime_error("invalid initialization");
  }
  erc20_address = *r;

  r = test::erc20::deploy_and_initialize_wallet(runtime_framework,
                                                erc20_address, 0);

  if (!r) {
    throw std::runtime_error("invalid initialization");
  }

  manager_address = *r;

  for (auto i = 0; i < 2; i++) {
    r = test::erc20::deploy_and_initialize_wallet(runtime_framework,
                                                  erc20_address, i + 1);
    if (!r) {
      throw std::runtime_error("invalid init wallet");
    }
    wallets.push_back(*r);
  }

  runtime_framework.commit_round();

  if (!test::erc20::initialize_erc20(runtime_framework, manager_address,
                                     erc20_address)) {
    throw std::runtime_error("invalid init erc20");
  }

  runtime_framework.commit_round();

  for (auto const &wallet : wallets) {
    if (!test::erc20::mint_to(runtime_framework, manager_address, wallet,
                              1'000'000)) {
      throw std::runtime_error("invalid mint");
    }
  }

  runtime_framework.commit_round();

  auto n_payments = state.range(0);

  auto wasmi_context_ptr = make_hogexec_wasmi_context();

  state.ResumeTiming();
  for (auto _ : state) {
    for (auto i = 0U; i < n_payments; i++) {
      if (!test::erc20::send_payment(runtime_framework, wallets[0], wallets[1],
                                     1)) {
        throw std::runtime_error("bad payment");
      }
    }
  }
  state.PauseTiming();
}

BENCHMARK(bm_erc20_2wallets)->RangeMultiplier(2)->Range(2, 2 << 10); // NOLINT

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
