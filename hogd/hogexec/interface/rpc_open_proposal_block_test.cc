#include <gtest/gtest.h>

#include <stdexcept>
#include <utility>
#include <format>

#include "exec_sched.hh"
#include "hogd/base/coropoll.h"
#include "hogd/base/types.h"
#include "hogd/hogexec/interface/rpc_from_coord.h"
#include "hogd/version.h"
#include "hogd/base/rpc_server.h"
#include "rpc_types.hh"
#include "sched_db.hh"
#include "sched_event.hh"
#include "sched_exec.hh"
#include "transaction.hh"
#include "x23/refcnt.h"
#include "x23/rpc.h"
#include "hogd/hogexec/runtime/native_contract_test_framework.h"
#include "x23/srpc.h"
#include "x23/vec.h"

namespace hogd {

class RpcOpenProposalBlockTest : public ::testing::Test {
public:
  void SetUp() override {
    // Start RPC server on a random port
    server_port_ = rpc_server_.start_server(0);
    
    // Create RPC client connected to the server
    auto server_addr = host_port_to_sockaddr("localhost", server_port_);
    auto client_fd = default_coropoll().tcpconnect(&server_addr)();
    if (!client_fd) {
      throw std::runtime_error("Failed to connect to RPC server");
    }

    client_ = x23::SRpcMulticlient<x23::RpcLogger>::make(server_addr)->client<ExecutorSched1>();

    auto db_server_addr = host_port_to_sockaddr("localhost", db_wrapper_.coord_port());
    auto db_client_fd = default_coropoll().tcpconnect(&db_server_addr)();
    if (!db_client_fd) {
      throw std::runtime_error("Failed to connect to DB server");
    }

    db_client_ = x23::SRpcMulticlient<x23::RpcLogger>::make(db_server_addr)->client<DbSched1>();
    auto db_res = db_client_.dbsched_load_genesis_block(GenesisBlock{});
    if (db_res != DbSchedResult::DbSchedResultOK) {
      throw std::runtime_error("Failed to load genesis block");
    }
  }
  
  void TearDown() override {
    rpc_server_.shut_down();
  }

protected:
  
  HogdbWrapper db_wrapper_;
  EventServerWrapper event_wrapper_;

  RpcServer<ExecutorMempoolRpc> rpc_server_{default_coropoll()};
  uint16_t server_port_ = 0;
  
  ExecutorSched1::client<x23::RefPtr<x23::SRpcMulticlient<x23::RpcLogger>>> client_;
  DbSched1::client<x23::RefPtr<x23::SRpcMulticlient<x23::RpcLogger>>> db_client_;
  
  ShardCard make_shard_card() {
    return ShardCard{
      .protocol = hogd::version::kHogdProtocol,
      .upid = {},
      .block_height = 0,
      .address_for_exec = std::format("localhost:{}", db_wrapper_.exec_port()).c_str(),
      .address_for_sched = std::format("localhost:{}", db_wrapper_.coord_port()).c_str()};
  }
  EventCard make_event_card() {
    return event_wrapper_.make_event_card();
  }
};

//
// Test that open_proposal_block returns SystemError before init().
//
TEST_F(RpcOpenProposalBlockTest, TestOpenProposalBlockBeforeInit) {
  BlockID test_block_id = 123;
  
  // Call open_proposal_block before init - should fail
  auto result = client_.executorsched_open_proposal_block(test_block_id);
  
  // Should return system error since executor is not initialized
  EXPECT_EQ(result, ExecResult::ExecResultSystemError);
}

//
// Test that open_proposal_block returns OK after init().
//
TEST_F(RpcOpenProposalBlockTest, TestOpenProposalBlockAfterInit) {
  BlockID test_block_id = 456;
  
  // Create a mock IntroExecResult for initialization
  IntroExecResult exec_result {
    .exec_id = 1,
    .shards = x23::Vec<ShardCard>{make_shard_card()},
    .event = make_event_card(),
  };
  
  // Initialize the executor with a shard pointing to our mock database server
  rpc_server_.impl_.init(std::move(exec_result));
  
  // Call open_proposal_block after init - should succeed
  auto result = client_.executorsched_open_proposal_block(test_block_id);
  
  // Should return OK since executor is now initialized
  EXPECT_EQ(result, ExecResult::ExecResultOK);
}

//
// Test open_proposal_block refcounting and finish_block behavior.
//
TEST_F(RpcOpenProposalBlockTest, TestOpenProposalBlockMultipleCalls) {
  BlockID test_block_id = 789;
  
  IntroExecResult exec_result;
  exec_result.exec_id = 1;
  
  
  exec_result.shards = x23::Vec<ShardCard>{make_shard_card()};
  exec_result.event = make_event_card();
  
  // Initialize the executor with a shard pointing to our mock database server
  rpc_server_.impl_.init(std::move(exec_result));
  
  // Call open_proposal_block multiple times - should all succeed
  auto result1 = client_.executorsched_open_proposal_block(test_block_id);
  auto result2 = client_.executorsched_open_proposal_block(test_block_id);
  auto result3 = client_.executorsched_open_proposal_block(test_block_id);

  // open corresponding db block
  auto db_res = db_client_.dbsched_create_snapshot(0, test_block_id, BlockExecutionMode::kBlockExecutionModeProposal);
  if (db_res != DbSchedResult::DbSchedResultOK) {
    throw std::runtime_error("Failed to open db block");
  }
  
  // All calls should return OK
  EXPECT_EQ(result1, ExecResult::ExecResultOK);
  EXPECT_EQ(result2, ExecResult::ExecResultOK);
  EXPECT_EQ(result3, ExecResult::ExecResultOK);

  client_.executorsched_finish_block(test_block_id);
  client_.executorsched_finish_block(test_block_id);

  EXPECT_EQ(client_.executorsched_propose_transaction(test_block_id, SignedTransaction{}).result(), ExecResult::ExecResultTxDeterministicError);

  client_.executorsched_finish_block(test_block_id);

  EXPECT_EQ(client_.executorsched_propose_transaction(test_block_id, SignedTransaction{}).result(), ExecResult::ExecResultSystemError);
}

} // namespace hogd
