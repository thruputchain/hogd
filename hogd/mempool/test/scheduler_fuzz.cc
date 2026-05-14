#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <memory>
#include <unordered_set>
#include <vector>

#include <openssl/crypto.h>

#include "hogd/base/test.h"
#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h"
#include "hogd/crypto/crypto.h"
#include "hogd/init.h"
#include "hogd/mempool/rpc_from_coord.h"
#include "hogd/mempool/scheduler.h"
#include "hogd/mempool/test/infra.h"
#include "hogd/mempool/test/txs.h"
#include "transaction.hh"
#include "types.hh"
#include "x23/vec.h"

namespace hogd {
namespace {

constexpr size_t kMaxOps = 64;
constexpr size_t kTxsPerAccount = 16;
constexpr size_t kMaxBatchSize = 8;
constexpr uint128_t kDefaultHogs = 5'000'000'000ULL * 16;

class FuzzInput {
public:
  FuzzInput(const uint8_t *data, size_t size) : data_(data), size_(size) {}

  bool empty() const { return offset_ >= size_; }

  uint8_t byte()
  {
    if (empty()) {
      return 0;
    }
    return data_[offset_++];
  }

  uint64_t bounded(uint64_t limit)
  {
    if (limit == 0) {
      return 0;
    }
    return integer() % limit;
  }

  uint64_t integer()
  {
    uint64_t value = 0;
    for (size_t i = 0; i < sizeof(value); i++) {
      value = (value << 8) | byte();
    }
    return value;
  }

private:
  const uint8_t *data_;
  size_t size_;
  size_t offset_ = 0;
};

SignedTransaction
make_tx(const std::vector<SignedTransactionBatch> &txs_by_account,
        FuzzInput &input)
{
  const size_t account_idx =
      input.bounded(MempoolTestInfra::kAccountAddresses.size());
  const SignedTransactionBatch &txs = txs_by_account[account_idx];
  SignedTransaction tx = txs[input.bounded(txs.size())];

  // Keep transactions structurally valid but vary the scheduler-relevant
  // fields. The expiry is always in the future so the harness can track cache
  // membership without modeling expiry-side removals.
  tx.tx.uniquifier.nonce = input.integer();
  tx.tx.uniquifier.block_expiry_height =
      std::numeric_limits<BlockHeight>::max() - input.bounded(1024);
  const GasAmount gas =
      static_cast<GasAmount>(1 + input.bounded(5'000'000'000ULL * 4));
  tx.tx.invocation.gas_limit = gas;
  return tx;
}

void
check_scheduled_batch(const x23::XdrVec<SignedTransactionHash> &hashes,
                      const x23::XdrVec<SignedTransaction> &txs)
{
  CHECK_EQ(hashes.size(), txs.size());
  std::unordered_set<SignedTransactionHash, ByteArrayHasher> seen;
  seen.reserve(hashes.size());
  for (size_t i = 0; i < hashes.size(); i++) {
    CHECK_EQ(hashes[i], sha256(txs[i].tx));
    CHECK(seen.insert(hashes[i]).second);
  }
}

void
check_scheduler_invariants(Scheduler &scheduler)
{
  CHECK_LE(scheduler.size(), scheduler.tx_cache_size());
}

void
set_account_hogs(MempoolTestInfra &server, Scheduler &scheduler,
                 const ContractAddress &account, FuzzInput &input)
{
  const uint128_t to_schedule =
      static_cast<uint128_t>(input.bounded(5'000'000'000ULL * 24));
  const uint128_t already_scheduled =
      static_cast<uint128_t>(input.bounded(5'000'000'000ULL * 8));
  const uint128_t received =
      static_cast<uint128_t>(input.bounded(5'000'000'000ULL * 8));

  server.set_hogs(account, kHogAgeGroupToSchedule, input.bounded(32),
                  to_schedule);
  server.set_hogs(account, kHogAgeGroupAlreadyScheduled, input.bounded(32),
                  already_scheduled);
  server.set_hogs(account, kHogAgeGroupReceived, input.bounded(32), received);
  scheduler.read_average_hog_ages({account})();
}

void
run_scheduler_fuzz_case(const uint8_t *data, size_t size)
{
  FuzzInput input(data, size);

  std::vector<fib_client_t> clients;
  std::vector<SignedTransactionBatch> txs_by_account;
  clients.reserve(MempoolTestInfra::kAccountAddresses.size());
  txs_by_account.reserve(MempoolTestInfra::kAccountAddresses.size());
  for (const ContractAddress &address : MempoolTestInfra::kAccountAddresses) {
    clients.emplace_back(address);
    txs_by_account.push_back(
        generate_unsigned_txs(clients.back(), kTxsPerAccount));
  }

  MempoolTestInfra server;
  server.mempool_coord_rpc().wait_for_auth_data(/*expected_height=*/0)();
  std::shared_ptr<Scheduler> scheduler = server.mempool_coord_rpc().scheduler();

  for (const ContractAddress &account : MempoolTestInfra::kAccountAddresses) {
    server.set_hogs(account, kHogAgeGroupToSchedule, /*age=*/0, kDefaultHogs);
    server.set_hogs(account, kHogAgeGroupAlreadyScheduled, /*age=*/0,
                    static_cast<uint128_t>(0));
    server.set_hogs(account, kHogAgeGroupReceived, /*age=*/0,
                    static_cast<uint128_t>(0));
  }

  std::unordered_set<SignedTransactionHash, ByteArrayHasher> cached_hashes;
  x23::XdrVec<SignedTransactionHash> last_scheduled_hashes;
  x23::XdrVec<SignedTransaction> last_scheduled_txs;
  BlockID pending_nomination = 0;
  BlockID next_nomination = 1;

  const size_t num_ops = std::min(kMaxOps, size);
  for (size_t op_idx = 0; op_idx < num_ops && !input.empty(); op_idx++) {
    switch (input.bounded(10)) {
    case 0:
    {
      x23::XdrVec<SignedTransaction> batch;
      const size_t batch_size = 1 + input.bounded(kMaxBatchSize);
      batch.reserve(batch_size);
      for (size_t i = 0; i < batch_size; i++) {
        batch.push_back(make_tx(txs_by_account, input));
      }
      scheduler->add_txs(batch)();
      for (const SignedTransaction &tx : batch) {
        cached_hashes.insert(sha256(tx.tx));
      }
      break;
    }
    case 1:
    {
      last_scheduled_hashes.clear();
      last_scheduled_txs.clear();
      scheduler->schedule(input.bounded(kMaxBatchSize + 1),
                          last_scheduled_hashes, last_scheduled_txs);
      check_scheduled_batch(last_scheduled_hashes, last_scheduled_txs);
      break;
    }
    case 2:
      if (pending_nomination == 0) {
        pending_nomination = next_nomination++;
        scheduler->start_pending_nomination(pending_nomination);
      }
      break;
    case 3:
      if (pending_nomination != 0) {
        scheduler->commit_pending_nomination(pending_nomination);
        pending_nomination = 0;
      }
      break;
    case 4:
      if (pending_nomination != 0) {
        scheduler->abandon_pending_nomination(pending_nomination);
        pending_nomination = 0;
      }
      break;
    case 5:
      if (!last_scheduled_hashes.empty()) {
        scheduler->reenqueue_txs(last_scheduled_hashes.begin(),
                                 last_scheduled_hashes.end());
      }
      break;
    case 6:
    {
      x23::Vec<SignedTransactionHash> hashes;
      for (const SignedTransactionHash &hash : last_scheduled_hashes) {
        if (cached_hashes.contains(hash) && input.bounded(2)) {
          hashes.push_back(hash);
        }
      }
      if (!hashes.empty()) {
        scheduler->remove_from_scheduler(
            x23::span<const SignedTransactionHash>(hashes))();
        for (const SignedTransactionHash &hash : hashes) {
          cached_hashes.erase(hash);
        }
      }
      break;
    }
    case 7:
    {
      HogsTransferred transfer;
      transfer.block = scheduler->current_block() + input.bounded(8);
      const size_t num_deltas =
          1 + input.bounded(MempoolTestInfra::kAccountAddresses.size());
      for (size_t i = 0; i < num_deltas; i++) {
        const ContractAddress &account =
            MempoolTestInfra::kAccountAddresses[input.bounded(
                MempoolTestInfra::kAccountAddresses.size())];
        transfer.deltas.push_back(
            Delta{.address = account,
                  .hog_delta = to_xdr(static_cast<int128_t>(
                      1 + input.bounded(5'000'000'000ULL * 4)))});
      }
      scheduler->process_transfers(transfer)();
      break;
    }
    case 8:
      if (!last_scheduled_txs.empty()) {
        scheduler->apply_committed_block_hog_ages(last_scheduled_txs,
                                                  scheduler->current_block() +
                                                      1)();
      }
      scheduler->increment_current_block();
      break;
    case 9:
    {
      const ContractAddress &account =
          MempoolTestInfra::kAccountAddresses[input.bounded(
              MempoolTestInfra::kAccountAddresses.size())];
      set_account_hogs(server, *scheduler, account, input);
      break;
    }
    default:
      break;
    }

    check_scheduler_invariants(*scheduler);
  }

  if (pending_nomination != 0) {
    scheduler->abandon_pending_nomination(pending_nomination);
  }
}

} // namespace
} // namespace hogd

extern "C" int
LLVMFuzzerInitialize(int *argc, char ***argv)
{
  std::atexit(OPENSSL_cleanup);

  if (hogd::hogd_init(*argc, *argv, /*do_print_version=*/false,
                      hogd::LogLevel::kError) != 0) {
    return 1;
  }
  return 0;
}

extern "C" int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
  if (size == 0) {
    return 0;
  }
  hogd::run_scheduler_fuzz_case(data, size);
  return 0;
}
