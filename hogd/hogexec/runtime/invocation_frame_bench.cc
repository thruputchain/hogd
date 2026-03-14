#include <benchmark/benchmark.h>

#include <cstdint>

#include "contract.hh"
#include "event.hh"
#include "hogd/base/types.h"
#include "hogd/hogexec/runtime/invocation_frame.h"
#include "hogd/hogexec/runtime/item/item_view.h"
#include "storage_item.hh"
#include "transaction.hh"
#include "types.hh"

namespace hogd {
namespace {

AddressAndKey
make_addrkey(uint32_t idx)
{
  ContractAddress address{};
  address[0] = static_cast<uint8_t>(idx);
  address[1] = static_cast<uint8_t>(idx >> 8);
  address[2] = static_cast<uint8_t>(idx >> 16);
  address[3] = static_cast<uint8_t>(idx >> 24);

  StorageKey key(StorageKeyType::kStorageKeyTypeContractData);
  key.contract_data_key() = {};
  key.contract_data_key()[0] = static_cast<uint8_t>(idx);
  key.contract_data_key()[1] = static_cast<uint8_t>(idx >> 8);
  key.contract_data_key()[2] = static_cast<uint8_t>(idx >> 16);
  key.contract_data_key()[3] = static_cast<uint8_t>(idx >> 24);
  return AddressAndKey{
      .address = address,
      .key = std::move(key),
  };
}

ItemView
make_item_view(uint32_t idx)
{
  BytestringItem item;
  item.bytestring_ = Bytestring{
      static_cast<uint8_t>(idx),
      static_cast<uint8_t>(idx >> 8),
      static_cast<uint8_t>(idx >> 16),
      static_cast<uint8_t>(idx >> 24),
  };

  ItemView view;
  auto res = view.set(item);
  benchmark::DoNotOptimize(res);
  return view;
}

void
populate_frame(InvocationFrame &frame, uint32_t start_idx, uint32_t count)
{
  for (uint32_t i = 0; i < count; ++i) {
    frame.add_item_view(make_addrkey(start_idx + i),
                        make_item_view(start_idx + i));
  }
}

void
bm_commit_successful_child_frame(benchmark::State &state)
{
  constexpr ContractAddress kParentAddress = ContractAddress{1};
  constexpr ContractAddress kChildAddress = ContractAddress{2};
  constexpr uint32_t kParentItems = 16;

  for (auto _ : state) {
    state.PauseTiming();
    InvocationFrame parent(kParentAddress, Calldata{},
                           std::to_underlying(InvokeFlag::kInvokeFlagNone));
    InvocationFrame child(kChildAddress, Calldata{},
                          std::to_underlying(InvokeFlag::kInvokeFlagNone));
    populate_frame(parent, 0, kParentItems);
    populate_frame(child, kParentItems, state.range(0));
    state.ResumeTiming();

    parent.commit_successful_child_frame(std::move(child));
    benchmark::ClobberMemory();
  }
}

BENCHMARK(bm_commit_successful_child_frame)->RangeMultiplier(4)->Range(8, 512);

} // namespace
} // namespace hogd
