#include <atomic>
#include <benchmark/benchmark.h>

#include <cstddef>
#include <cstdio>
#include <cstring>
#include <future>
#include <memory>
#include <print>
#include <random>
#include <span>
#include <string>
#include <vector>

#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "hogd/base/worker_pool.h"
#include "hogd/crypto/crypto.h"
#include "hogd/mempool/mempool.h"
#include "types.hh"

namespace hogd {

void
hello_world(benchmark::State &state)
{}
BENCHMARK(hello_world)->Unit(benchmark::kMillisecond);

namespace {

struct Init {
  Init()
  {
    // Initialize logging. This is needed for the CHECK macros.
    log_init(std::make_unique<ConsoleLogger>());
  }
};

// This runs [Init]'s constructor at program startup to do initialization.
Init init;

} // namespace

} // namespace hogd

BENCHMARK_MAIN();
