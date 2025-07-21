// mempool

#pragma once

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
#include "hogd/container/merkle/merkle_trie.h"
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

class Validator {
public:
  Validator();
  ~Validator() = default;

  bool validate(const SHA256Hash &expected_root_hash,
                const std::vector<SignedTransaction> &txs);

  // Disable copy and move.
  Validator(const Validator &) = delete;
  Validator &operator=(const Validator &) = delete;
  Validator(Validator &&) noexcept = delete;
  Validator &operator=(Validator &&) noexcept = delete;

private:
  static constexpr std::string kTestContractFileName = "fibonacci.wasm";
  static constexpr uint16_t kExecutorPort = 10000;
  static constexpr uint16_t kCoordinatorPort = 10001;
  static constexpr size_t kNumDbPoolThreads = 16;
  static constexpr size_t kNumValidatorPoolThreads = 16;

  using coord_client_t =
      DbCoord1::client<x23::RpcStream<x23::RpcLogger>::ClientRef>;

  SystemCallHandler make_handler(SignedTransaction const &signed_tx,
                                 TransactionID txid);

  void advance_block();

  TransactionID allocate_txid();

  bool execute_txs(const std::vector<SignedTransaction> &txs,
                   MerkleTriePrefix &trie);

  // Server.
  x23::ThreadPool db_pool_;
  InMemoryDatabase in_memory_db_;
  StorageCostModel cost_model_;
  BlockManager block_manager_;
  RPCDbExecutorServer db_executor_server_;
  DbCoordServer db_coordinator_server_;

  // Client.
  WorkerPool validator_pool_;
  x23::CoroPoll cp_ = x23::CoroPoll(db_pool_);
  std::unique_ptr<DatabaseProxy> db_proxy_;
  std::unique_ptr<StorageSnapshotCache> snapshot_cache_;
  std::unique_ptr<coord_client_t> coord_client_;
};

} // namespace hogd
