#include <benchmark/benchmark.h>

#include <cstdint>
#include <memory>
#include <unordered_map>

#include "contract.hh"
#include "hogd/crypto/crypto.h"
#include "hogd/hogexec/runtime/invocation_frame.h"

namespace hogd {
namespace {

ContractAddress
make_contract_address(uint32_t id)
{
  ContractAddress address{};
  address[0] = static_cast<uint8_t>(id);
  address[1] = static_cast<uint8_t>(id >> 8);
  address[2] = static_cast<uint8_t>(id >> 16);
  address[3] = static_cast<uint8_t>(id >> 24);
  return address;
}

struct DeployBatchFixture {
  InvocationFrame frame{make_contract_address(0), Calldata{}, 0};
  std::unordered_map<SHA256Hash, std::unique_ptr<const Contract>>
      newly_created_contracts;

  explicit DeployBatchFixture(int num_contracts)
  {
    newly_created_contracts.reserve(static_cast<size_t>(num_contracts));
    for (int i = 0; i < num_contracts; ++i) {
      Contract contract(ContractType::kWasmi);
      contract.wasmi_contract().wasm = {
          static_cast<uint8_t>(i),
          static_cast<uint8_t>(i >> 8),
          static_cast<uint8_t>(i >> 16),
          static_cast<uint8_t>(i >> 24),
      };

      auto hash = sha256(contract);
      frame.deploy_contract(
          make_contract_address(static_cast<uint32_t>(i + 1)),
          DeployedContractHash{.hash = hash,
                               .contract_size = static_cast<uint32_t>(
                                   contract.wasmi_contract().wasm.size()),
                               .contract_type = ContractType::kWasmi});
      newly_created_contracts.emplace(hash, std::make_unique<const Contract>(
                                                contract));
    }
  }
};

void
bm_deployment_commit_batch(benchmark::State &state)
{
  DeployBatchFixture fixture(static_cast<int>(state.range(0)));

  for (auto _ : state) {
    NewContractBatch out;
    fixture.frame.append_new_contract_batch(fixture.newly_created_contracts,
                                            out);
    benchmark::DoNotOptimize(out);
  }
}
BENCHMARK(bm_deployment_commit_batch)
    ->RangeMultiplier(2)
    ->Range(1, 512)
    ->Unit(benchmark::kNanosecond);

} // namespace
} // namespace hogd
