#include <benchmark/benchmark.h>

#include <memory>
#include <unordered_map>
#include <utility>

#include "contract.hh"
#include "hogd/base/types.h"
#include "hogd/hogexec/runtime/multiplexed_context.h"
#include "hogd/hogexec/runtime/runtime.h"

namespace hogd {

namespace {

class DummyRuntime final : public Runtime {
public:
  DummyRuntime() : Runtime(make_new_host_call_context(nullptr)) {}

  [[nodiscard]] uint64_t get_available_gas() const noexcept override
  {
    return 0;
  }

  void set_available_gas(uint64_t /*gas*/) noexcept override {}

  std::span<std::byte> get_memory() noexcept override { return {}; }

protected:
  InvokeStatus<void> invoke_internal(std::string_view /*method_name*/) override
  {
    return {};
  }
};

class DummyContext final : public RuntimeContext {
public:
  bool verify_contract(Contract const & /*contract*/) const override
  {
    return true;
  }

  SystemStatus<std::unique_ptr<Runtime>>
  make_new_runtime(DeployedContractHash const & /*deployed_contract_hash*/,
                   SystemCallHandler * /*syscall_handler*/,
                   bool /*do_contract_verify*/) override
  {
    return std::make_unique<DummyRuntime>();
  }
};

std::unordered_map<ContractType, std::pair<RuntimeContext *, bool>>
make_context_map()
{
  std::unordered_map<ContractType, std::pair<RuntimeContext *, bool>> contexts;
  contexts.emplace(ContractType::kWasmi,
                   std::pair{new DummyContext(), true}); // NOLINT
  contexts.emplace(ContractType::kNative,
                   std::pair{new DummyContext(), true}); // NOLINT
  return contexts;
}

void
bm_multiplexed_context_construct_legacy(benchmark::State &state)
{
  for (auto _ : state) {
    auto contexts = make_context_map();
    benchmark::DoNotOptimize(contexts);
    MultiplexedContext context(std::move(contexts));
    benchmark::DoNotOptimize(context);
  }
}
BENCHMARK(bm_multiplexed_context_construct_legacy)
    ->Unit(benchmark::kNanosecond);

void
bm_multiplexed_context_construct_direct(benchmark::State &state)
{
  for (auto _ : state) {
    MultiplexedContext context({.context = new DummyContext(), .owned = true},
                               {.context = new DummyContext(), .owned = true});
    benchmark::DoNotOptimize(context);
  }
}
BENCHMARK(bm_multiplexed_context_construct_direct)
    ->Unit(benchmark::kNanosecond);

void
bm_multiplexed_context_make_new_runtime(benchmark::State &state)
{
  MultiplexedContext context(make_context_map());
  DeployedContractHash wasmi_hash{
      .contract_type = ContractType::kWasmi,
  };
  DeployedContractHash native_hash{
      .contract_type = ContractType::kNative,
  };

  for (auto _ : state) {
    auto wasmi_runtime =
        context.make_new_runtime(wasmi_hash, nullptr, false).value();
    auto native_runtime =
        context.make_new_runtime(native_hash, nullptr, false).value();
    benchmark::DoNotOptimize(wasmi_runtime);
    benchmark::DoNotOptimize(native_runtime);
  }
}
BENCHMARK(bm_multiplexed_context_make_new_runtime)
    ->Unit(benchmark::kNanosecond);

void
bm_multiplexed_context_verify_contract(benchmark::State &state)
{
  MultiplexedContext context(make_context_map());
  Contract wasmi_contract(ContractType::kWasmi);
  Contract native_contract(ContractType::kNative);

  for (auto _ : state) {
    benchmark::DoNotOptimize(context.verify_contract(wasmi_contract));
    benchmark::DoNotOptimize(context.verify_contract(native_contract));
  }
}
BENCHMARK(bm_multiplexed_context_verify_contract)->Unit(benchmark::kNanosecond);

} // namespace

} // namespace hogd

BENCHMARK_MAIN();
