#include <memory>
#include <string_view>
#include <unordered_map>

#include <benchmark/benchmark.h>

#include "contract.hh"
#include "hogd/base/types.h"
#include "hogd/crypto/crypto.h"
#include "hogd/hogexec/runtime/item/storage_snapshot_cache.h"
#include "hogd/hogexec/runtime/native_contract_test_framework.h"
#include "hogd/init.h"

namespace hogd {
namespace {

using NewContractMap =
    std::unordered_map<SHA256Hash, std::unique_ptr<const Contract>>;

const Contract *
lookup_newly_created_contract(NewContractMap const &contracts,
                              SHA256Hash const &hash)
{
  auto it = contracts.find(hash);
  if (it == contracts.end()) {
    return nullptr;
  }
  return it->second.get();
}

SystemStatus<const Contract *>
resolve_contract_snapshot_first(StorageSnapshotCache &snapshot_cache,
                                NewContractMap const &contracts,
                                SHA256Hash const &hash)
{
  auto snapshot_res = snapshot_cache.try_get_contract_by_hash(hash);
  if (!snapshot_res.has_value()) {
    return snapshot_res.error();
  }
  if (*snapshot_res != nullptr) {
    return *snapshot_res;
  }
  if (auto *local_contract = lookup_newly_created_contract(contracts, hash)) {
    return local_contract;
  }
  return nullptr;
}

SystemStatus<const Contract *>
resolve_contract_local_first(StorageSnapshotCache &snapshot_cache,
                             NewContractMap const &contracts,
                             SHA256Hash const &hash)
{
  if (auto *local_contract = lookup_newly_created_contract(contracts, hash)) {
    return local_contract;
  }
  return snapshot_cache.try_get_contract_by_hash(hash);
}

struct ResolveContractBenchState {
  NativeContractTestFramework framework;
  NewContractMap contracts;
  SHA256Hash contract_hash;

  ResolveContractBenchState() : framework(false)
  {
    auto contract = NativeContractTestFramework::make_native_contract(9999);
    contract_hash = sha256(std::string_view{"resolve-contract-bench-local"});
    contracts.emplace(contract_hash,
                      std::make_unique<const Contract>(*contract));

    auto warm =
        framework.get_storage_snapshot_cache()->try_get_contract_by_hash(
            contract_hash);
    CHECK(warm.has_value());
    if (*warm != nullptr) {
      panic("expected contract cache warmup miss");
    }
  }
};

void
bm_resolve_contract_snapshot_first(benchmark::State &state)
{
  ResolveContractBenchState bench_state;
  auto &snapshot_cache = *bench_state.framework.get_storage_snapshot_cache();

  for (auto _ : state) {
    auto res =
        resolve_contract_snapshot_first(snapshot_cache, bench_state.contracts,
                                        bench_state.contract_hash);
    benchmark::DoNotOptimize(res);
    CHECK(res.has_value());
    if (*res == nullptr) {
      panic("expected local contract hit");
    }
  }
}
BENCHMARK(bm_resolve_contract_snapshot_first)->Unit(benchmark::kNanosecond);

void
bm_resolve_contract_local_first(benchmark::State &state)
{
  ResolveContractBenchState bench_state;
  auto &snapshot_cache = *bench_state.framework.get_storage_snapshot_cache();

  for (auto _ : state) {
    auto res =
        resolve_contract_local_first(snapshot_cache, bench_state.contracts,
                                     bench_state.contract_hash);
    benchmark::DoNotOptimize(res);
    CHECK(res.has_value());
    if (*res == nullptr) {
      panic("expected local contract hit");
    }
  }
}
BENCHMARK(bm_resolve_contract_local_first)->Unit(benchmark::kNanosecond);

} // namespace
} // namespace hogd

int
main(int argc, char **argv)
{
  auto ret = hogd::hogd_init(argc, argv, false);
  if (ret != 0) {
    return ret;
  }

  benchmark::Initialize(&argc, argv);
  benchmark::RunSpecifiedBenchmarks();
  benchmark::Shutdown();
  return 0;
}
