#include <benchmark/benchmark.h>

#include <cstdint>
#include <cstdlib>
#include <vector>

#include "contract.hh"
#include "ffi_enums.hh"
#include "hogd/hogexec/runtime/invocation_frame.h"
#include "hogd/init.h"

namespace hogd {

namespace {

ContractAddress
make_contract_address(uint64_t seed)
{
  ContractAddress out{};
  auto *bytes = reinterpret_cast<unsigned char *>(out.data());
  for (size_t i = 0; i < out.size(); ++i) {
    bytes[i] = static_cast<unsigned char>((seed + i) & 0xffU);
  }
  return out;
}

SHA256Hash
make_hash(uint64_t seed)
{
  SHA256Hash out{};
  auto *bytes = reinterpret_cast<unsigned char *>(out.data());
  for (size_t i = 0; i < out.size(); ++i) {
    bytes[i] = static_cast<unsigned char>(((seed * 17U) + i) & 0xffU);
  }
  return out;
}

void
bench_lookup_locally_deployed_contract_recent_hit(benchmark::State &state)
{
  const int depth = static_cast<int>(state.range(0));
  std::vector<InvocationFrame> frames;
  frames.reserve(depth);

  for (int i = 0; i < depth; ++i) {
    frames.emplace_back(make_contract_address(static_cast<uint64_t>(i)),
                        Calldata{},
                        std::to_underlying(InvokeFlag::kInvokeFlagNone));
  }

  const auto deployed_address =
      make_contract_address(static_cast<uint64_t>(depth) + 1U);
  frames.back().deploy_contract(deployed_address,
                                DeployedContractHash{
                                    .hash =
                                        make_hash(static_cast<uint64_t>(depth)),
                                    .contract_size = 128,
                                    .contract_type = ContractType::kWasmi,
                                });

  for (auto _ : state) {
    benchmark::DoNotOptimize(
        detail::lookup_locally_deployed_contract(frames, deployed_address));
  }
}

BENCHMARK(bench_lookup_locally_deployed_contract_recent_hit)
    ->Arg(1)
    ->Arg(4)
    ->Arg(16)
    ->Arg(64)
    ->Arg(256)
    ->Arg(1024);

} // namespace

} // namespace hogd

int
main(int argc, char *argv[])
{
  auto ret = hogd::hogd_init(argc, argv);
  if (ret != 0) {
    return EXIT_FAILURE;
  }

  benchmark::Initialize(&argc, argv);
  benchmark::RunSpecifiedBenchmarks();
  return EXIT_SUCCESS;
}
