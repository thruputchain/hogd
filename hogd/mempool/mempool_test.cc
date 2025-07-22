#include <cstddef>
#include <cstdio>
#include <cstring>
#include <future>
#include <gtest/gtest.h>
#include <iostream>

#include <map>
#include <memory>
#include <print>
#include <random>
#include <set>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "hogd/base/worker_pool.h"
#include "hogd/crypto/crypto.h"
#include "hogd/mempool/mempool.h"
#include "types.hh"

namespace hogd {

namespace {

using fib_client_t = ContractTraitClient<Fibonacci>;

std::vector<SignedTransaction>
generate_txs(fib_client_t &fib_client, size_t num_txs)
{
  constexpr char kProgramName[] = "slow_fibonacci";
  constexpr uint32_t kFibonacciN = 10;

  std::vector<SignedTransaction> txs;
  txs.reserve(num_txs);
  for (size_t i = 0; i < num_txs; i++) {
    SignedTransaction tx{
        .tx =
            Transaction{.invocation = fib_client.make_invocation<kProgramName>(
                            UINT64_MAX, kFibonacciN)}};
    txs.push_back(tx);
  }
  return txs;
}

} // namespace

TEST(Mempool, HelloWorld)
{
  constexpr size_t kNumTxs = 100;

  fib_client_t fib_client(ContractAddress{});
  std::vector<SignedTransaction> txs = generate_txs(fib_client, kNumTxs);

  {
    Validator validator;
    SHA256Hash empty{};
    EXPECT_FALSE(validator.validate(/*expected_root_hash=*/empty, txs));
  }

  {
    // We reconstruct the validator each time we attempt to validate [txs]
    // because [validate()] *adds* the txs to the block rather than overwrite
    // the existing txs in the block.
    Validator validator;
    HexArray expected(
        "87eb620d9086e7df7899c4c016d3696cc198682e70105f18a4b90be1fd563564");
    EXPECT_TRUE(
        validator.validate(/*expected_root_hash=*/expected.to_sha256(), txs));
  }
}

} // namespace hogd

int
main(int argc, char *argv[])
{
  testing::InitGoogleTest(&argc, argv);
  // Initialize logging. This is needed for the CHECK macros.
  log_init(std::make_unique<hogd::ConsoleLogger>());

  return RUN_ALL_TESTS();
}
