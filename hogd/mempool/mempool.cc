#include "hogd/mempool/mempool.h"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <future>
#include <memory>
#include <optional>
#include <print>
#include <stdexcept>
#include <string>
#include <unistd.h>
#include <vector>

#include "db_coord.hh"
#include "hogd/base/types.h"
#include "hogd/base/worker_pool.h"
#include "hogd/crypto/crypto.h"
#include "hogd/db_interface/block_manager.h"
#include "hogd/db_interface/db_coord_server.h"
#include "hogd/db_interface/db_executor_server.h"
#include "hogd/hogexec/runtime/contract_trait.h"
#include "hogd/hogexec/runtime/database_proxy.h"
#include "hogd/hogexec/runtime/execution_mode.h"
#include "hogd/hogexec/runtime/storage_snapshot_cache.h"
#include "hogd/hogexec/runtime/syscall_handler.h"
#include "hogd/hogexec/runtime/test_load_wasm.h"
#include "hogd/init.h"
#include "hogd/mockdb/in_memory_database.h"
#include "hogd/storage_cost/storage_cost_model.h"
#include "hogd/storage_item/storage_item_wrapper.h"
#include "rpc_types.hh"
#include "test_contracts/integration_test_contracts/fibonacci/fibonacci.h"
#include "transaction.hh"
#include "types.hh"
#include "x23/async.h"
#include "x23/event.h"
#include "x23/rpc.h"
#include "x23/threadpool.h"

namespace hogd {

namespace {

void
setup_grant_storage_token(InMemoryDatabase &in_memory_db,
                          StorageCostModel const &cost_model,
                          ContractAddress const &address, int128_t amount)
{
  CHECK_GE(amount, 0);
  CHECK_LE(amount, int128_t{kHogDivisibility} * int128_t{kTotalHogSupply});

  AddressAndKey addrkey = {
      .address = address,
      .key = StorageKey(StorageKeyType::kStorageKeyTypeFreeNativeTokenBalance)};

  in_memory_db.write_item(
      addrkey, std::make_shared<const StorageItemWrapper>(amount, &cost_model));
}

} // namespace

Validator::Validator(size_t num_threads)
  : db_pool_(x23::ThreadPool(num_threads)), cost_model_(12000, 1, 5, 100, 5),
    block_manager_(0, &in_memory_db_),
    db_executor_server_(&block_manager_, kExecutorPort),
    db_coordinator_server_(&block_manager_)
{
  ContractAddress contract_addr;
  std::shared_ptr<Contract> contract =
      test::load_test_contract(kTestContractFileName);

  in_memory_db_.write_contract(sha256(*contract), contract);
  in_memory_db_.write_deployment(contract_addr, sha256(*contract));

  setup_grant_storage_token(in_memory_db_, cost_model_, contract_addr,
                            1'000'000'000);

  std::println("Starting executor server on port {}", kExecutorPort);
  std::println("Starting coord server on port {}", kCoordinatorPort);
  db_coordinator_server_.start_server(kCoordinatorPort);
}

bool
Validator::validate(const SHA256Hash &expected_root_hash,
                    const std::vector<SignedTransaction> &txs)
{
  // TODO(jhumphri):
  // 1. Send the txs to the executors and get the results.
  // 2. Throw out any txs that fail?
  // 3. Build a Merkle Trie of the txs.
  // 4. Compare the hash of the Merkle Trie root with [expected_root_hash].
  //
  // Follow up:
  // 1. We can probably build the Merkle Trie while the executors are executing
  //    the txs. If a tx fails, we can remove it from the Merkle Trie. This is
  //    likely more efficient than building the Merkle Trie only after we know
  //    which txs succeeded.
  // 2. Experiment with different scheduling policies for txs. We want to model
  //    txs of different lengths and multiple blocks validated at the same time.
  return false;
}

} // namespace hogd
