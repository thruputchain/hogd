#include <benchmark/benchmark.h>

#include <cstdint>
#include <memory>
#include <unordered_map>

#include "contract.hh"
#include "hogd/base/xdr_util.h" // IWYU pragma: keep
#include "hogd/crypto/crypto.h"

namespace hogd {
namespace {

Contract
make_bench_contract(uint32_t id)
{
  Contract contract(ContractType::kNative);
  contract.native_contract_id() =
      static_cast<HogexecPrecompiledNativeContracts>(id);
  return contract;
}

std::vector<Contract>
make_bench_contracts(int contract_count)
{
  std::vector<Contract> contracts;
  contracts.reserve(contract_count);
  for (int i = 0; i < contract_count; ++i) {
    contracts.push_back(make_bench_contract(static_cast<uint32_t>(i)));
  }
  return contracts;
}

auto
make_newly_created_contracts_legacy(std::vector<Contract> const &contracts)
{
  std::unordered_map<SHA256Hash, std::unique_ptr<const Contract>> out;
  for (auto const &contract : contracts) {
    out.emplace(sha256(contract), std::make_unique<const Contract>(contract));
  }
  return out;
}

void
bm_newly_created_contracts_build_legacy(benchmark::State &state)
{
  auto contracts = make_bench_contracts(state.range(0));

  for (auto _ : state) {
    auto out = make_newly_created_contracts_legacy(contracts);
    benchmark::DoNotOptimize(out.size());
    benchmark::ClobberMemory();
  }
}

auto
make_newly_created_contracts_reserved(std::vector<Contract> const &contracts)
{
  std::unordered_map<SHA256Hash, std::unique_ptr<const Contract>> out;
  out.reserve(contracts.size());
  for (auto const &contract : contracts) {
    out.emplace(sha256(contract), std::make_unique<const Contract>(contract));
  }
  return out;
}

void
bm_newly_created_contracts_build_reserved(benchmark::State &state)
{
  auto contracts = make_bench_contracts(state.range(0));

  for (auto _ : state) {
    auto out = make_newly_created_contracts_reserved(contracts);
    benchmark::DoNotOptimize(out.size());
    benchmark::ClobberMemory();
  }
}

BENCHMARK(bm_newly_created_contracts_build_legacy)
    ->Arg(0)
    ->Arg(1)
    ->Arg(4)
    ->Arg(16)
    ->Arg(64)
    ->Arg(256);

BENCHMARK(bm_newly_created_contracts_build_reserved)
    ->Arg(0)
    ->Arg(1)
    ->Arg(4)
    ->Arg(16)
    ->Arg(64)
    ->Arg(256);

} // namespace
} // namespace hogd

BENCHMARK_MAIN();
