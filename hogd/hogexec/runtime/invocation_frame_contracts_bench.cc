#include <benchmark/benchmark.h>

#include <cstddef>
#include <cstdint>
#include <cstdlib>

#include "contract.hh"
#include "hogd/hogexec/runtime/invocation_frame.h"

namespace hogd {

namespace {

constexpr ContractAddress
make_contract_address(uint32_t id)
{
  ContractAddress address{};
  address[0] = static_cast<uint8_t>(id);
  address[1] = static_cast<uint8_t>(id >> 8);
  address[2] = static_cast<uint8_t>(id >> 16);
  address[3] = static_cast<uint8_t>(id >> 24);
  return address;
}

SHA256Hash
make_hash(uint32_t id)
{
  SHA256Hash hash{};
  hash[0] = static_cast<uint8_t>(id);
  hash[1] = static_cast<uint8_t>(id >> 8);
  hash[2] = static_cast<uint8_t>(id >> 16);
  hash[3] = static_cast<uint8_t>(id >> 24);
  return hash;
}

void
bm_get_committed_contracts(benchmark::State &state)
{
  InvocationFrame frame(make_contract_address(0), Calldata{},
                        std::to_underlying(InvokeFlag::kInvokeFlagNone));

  for (uint32_t i = 0; i < static_cast<uint32_t>(state.range(0)); ++i) {
    auto hash = make_hash(i + 1);
    frame.deploy_contract(make_contract_address(i + 1),
                          DeployedContractHash{.hash = hash,
                                               .contract_size = 0,
                                               .contract_type =
                                                   ContractType::kWasmi});
  }

  for (auto _ : state) {
    auto committed = frame.get_committed_contracts();
    benchmark::DoNotOptimize(committed);
  }
}
BENCHMARK(bm_get_committed_contracts)
    ->RangeMultiplier(2)
    ->Range(8, 2048)
    ->Unit(benchmark::kNanosecond); // NOLINT

} // namespace

} // namespace hogd

int
main(int argc, char *argv[])
{
  benchmark::Initialize(&argc, argv);
  benchmark::RunSpecifiedBenchmarks();
  return EXIT_SUCCESS;
}
