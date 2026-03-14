#include <cstddef>
#include <cstdint>
#include <vector>

#include "benchmark/benchmark.h"
#include "contract.hh"
#include "ffi_enums.hh"
#include "hogd/base/types.h"
#include "hogd/hogexec/runtime/invocation_frame.h"
#include "hogd/hogexec/runtime/item/item_view.h"
#include "storage_item.hh"
#include "types.hh"

namespace hogd {

void
collect_storage_deltas_legacy(InvocationFrame const &frame,
                              x23::Vec<FlaggedStorageDelta, 0xffffffff> &out)
{
  for (auto const &[addrkey, item_view] : frame.modified_items_) {
    auto [delta, flags] = item_view.get_delta();
    if (delta) {
      out.push_back(FlaggedStorageDelta{
          .addrkey = addrkey,
          .delta = *delta,
          .flags = flags,
      });
    }
  }

  for (auto const &[addr, deployment] : frame.new_contract_deployments_) {
    AddressAndKey addrkey = {
        .address = addr,
        .key = StorageKey(StorageKeyType::kStorageKeyTypeAddressMetadata)};
    StorageDelta delta(StorageDeltaType::kDeltaTypeDeployContract);
    delta.deployed_contract_hash() = deployment;
    out.push_back(FlaggedStorageDelta{
        .addrkey = addrkey,
        .delta = delta,
        .flags = std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagNone),
    });
  }
}

namespace {

AddressAndKey
make_addrkey(uint64_t id)
{
  StorageKey key(StorageKeyType::kStorageKeyTypeContractData);
  key.contract_data_key() =
      StorageKeyContractData{static_cast<uint8_t>(id),
                             static_cast<uint8_t>(id >> 8)};
  return AddressAndKey{
      .address = ContractAddress{static_cast<uint8_t>(id),
                                 static_cast<uint8_t>(id >> 8)},
      .key = key,
  };
}

ContractAddress
make_contract_address(uint64_t id)
{
  return ContractAddress{static_cast<uint8_t>(id),
                         static_cast<uint8_t>(id >> 8)};
}

DeployedContractHash
make_deployment_hash(uint64_t id)
{
  DeployedContractHash out{
      .hash =
          SHA256Hash{static_cast<uint8_t>(id), static_cast<uint8_t>(id >> 8)},
      .contract_size = 0,
      .contract_type = ContractType::kNative,
  };
  return out;
}

BytestringItem
make_bytestring_item(uint64_t id)
{
  BytestringItem item;
  item.bytestring_ =
      Bytestring{static_cast<uint8_t>(id), static_cast<uint8_t>(id >> 8)};
  return item;
}

InvocationFrame
make_frame(size_t modified_item_count, size_t deployment_count)
{
  InvocationFrame frame(make_contract_address(0), Calldata{}, 0);

  for (size_t i = 0; i < modified_item_count; ++i) {
    ItemView item_view;
    auto st = item_view.set(make_bytestring_item(i));
    benchmark::DoNotOptimize(st);
    frame.add_item_view(make_addrkey(i), std::move(item_view));
  }

  for (size_t i = 0; i < deployment_count; ++i) {
    frame.deploy_contract(make_contract_address(i + modified_item_count + 1),
                          make_deployment_hash(i));
  }

  return frame;
}

void
run_collect_storage_deltas_bench(benchmark::State &state, bool use_legacy_impl)
{
  size_t const modified_item_count = static_cast<size_t>(state.range(0));
  size_t const deployment_count = modified_item_count / 4;
  auto frame = make_frame(modified_item_count, deployment_count);

  for (auto _ : state) {
    x23::Vec<FlaggedStorageDelta, 0xffffffff> deltas;
    if (use_legacy_impl) {
      collect_storage_deltas_legacy(frame, deltas);
    }
    else {
      frame.collect_storage_deltas(deltas);
    }
    benchmark::DoNotOptimize(deltas);
  }

  state.SetItemsProcessed(
      state.iterations() *
      static_cast<int64_t>(modified_item_count + deployment_count));
}

void
bm_collect_storage_deltas_legacy(benchmark::State &state)
{
  run_collect_storage_deltas_bench(state, true);
}

void
bm_collect_storage_deltas(benchmark::State &state)
{
  run_collect_storage_deltas_bench(state, false);
}

BENCHMARK(bm_collect_storage_deltas_legacy)
    ->Arg(32)
    ->Arg(128)
    ->Arg(512)
    ->Arg(2048);
BENCHMARK(bm_collect_storage_deltas)->Arg(32)->Arg(128)->Arg(512)->Arg(2048);

} // namespace

} // namespace hogd

BENCHMARK_MAIN();
