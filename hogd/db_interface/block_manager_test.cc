#include <gtest/gtest.h>

#include <memory>
#include <utility>

#include "contract.hh"
#include "genesis.hh"
#include "hogd/base/network_config_params.h"
#include "hogd/base/types.h"
#include "hogd/db_interface/block_manager.h"
#include "hogd/genesis/genesis_configuration.h"
#include "hogd/init.h"
#include "hogd/mockdb/in_memory_database.h"
#include "hogd/storage_cost/storage_cost_model.h"
#include "network_config.hh"
#include "rpc_types.hh"
#include "types.hh"

namespace hogd {

TEST(BlockManagerTest, CommitCleanup)
{
  InMemoryDatabase db;
  BlockManager m(0, &db);
  m.set_cost_model(StorageCostModel::default_cost_model());
  // block 0 doesn't have state that can be manipulated;
  // it's the root of the block tree, and is already committed.
  EXPECT_FALSE(m.get_block_data(0));
  EXPECT_FALSE(m.get_block_data(1));

  EXPECT_TRUE(
      m.add_child(0, 1, BlockExecutionMode::kBlockExecutionModeProposal));
  EXPECT_TRUE(m.get_block_data(1));

  // block 10 doesn't exist
  EXPECT_FALSE(
      m.add_child(10, 11, BlockExecutionMode::kBlockExecutionModeProposal));
  EXPECT_FALSE(m.get_block_data(11));

  // 2 and 3 should get cleaned up by commit 1
  // 2 gets cleaned up, even though it's been closed (but not committed).
  EXPECT_TRUE(
      m.add_child(0, 2, BlockExecutionMode::kBlockExecutionModeProposal));
  auto b2 = m.get_block_data(2);
  b2->close_block();
  EXPECT_TRUE(
      m.add_child(2, 3, BlockExecutionMode::kBlockExecutionModeProposal));

  // children of 2 get cleaned up by commit 1
  EXPECT_TRUE(
      m.add_child(2, 4, BlockExecutionMode::kBlockExecutionModeProposal));
  EXPECT_TRUE(
      m.add_child(2, 5, BlockExecutionMode::kBlockExecutionModeProposal));
  // can't add children to 3, because it hasn't closed yet
  EXPECT_FALSE(
      m.add_child(3, 6, BlockExecutionMode::kBlockExecutionModeProposal));

  EXPECT_TRUE(m.get_block_data(2));
  EXPECT_TRUE(m.get_block_data(3));

  m.get_block_data(1)->close_block();
  m.commit_block(1);

  EXPECT_FALSE(m.get_block_data(2));
  EXPECT_FALSE(m.get_block_data(3));
}

TEST(BlockManagerTest, ChildOfCommittedBlock)
{
  InMemoryDatabase db;
  BlockManager m(0, &db);
  m.set_cost_model(StorageCostModel::default_cost_model());
  EXPECT_TRUE(
      m.add_child(0, 1, BlockExecutionMode::kBlockExecutionModeProposal));
  ASSERT_TRUE(m.get_block_data(1));
  m.get_block_data(1)->close_block();
  EXPECT_TRUE(m.commit_block(1));
  EXPECT_TRUE(m.get_block_data(1));

  EXPECT_TRUE(
      m.add_child(1, 2, BlockExecutionMode::kBlockExecutionModeProposal));
  EXPECT_TRUE(m.get_block_data(2));
  m.get_block_data(2)->close_block();

  EXPECT_FALSE(
      m.get_block_data(2)->get_snapshot_view()->get_snapshot_deployment(
          ContractAddress{}));

  EXPECT_TRUE(
      m.add_child(1, 3, BlockExecutionMode::kBlockExecutionModeProposal));
  EXPECT_TRUE(m.get_block_data(3));
  m.get_block_data(3)->close_block();

  EXPECT_TRUE(m.commit_block(2));
  EXPECT_FALSE(m.get_block_data(3));
}

TEST(BlockManagerTest, AddSequenceOfChildren)
{
  InMemoryDatabase db;
  BlockManager m(0, &db);
  m.set_cost_model(StorageCostModel::default_cost_model());
  EXPECT_TRUE(
      m.add_child(0, 1, BlockExecutionMode::kBlockExecutionModeProposal));
  auto b1 = m.get_block_data(1);
  ASSERT_TRUE(b1);

  EXPECT_FALSE(m.commit_block(1));
  b1->close_block();
  EXPECT_TRUE(m.commit_block(1));

  EXPECT_TRUE(
      m.add_child(1, 2, BlockExecutionMode::kBlockExecutionModeProposal));
  auto b2 = m.get_block_data(2);
  ASSERT_TRUE(b2);
  b2->close_block();
  EXPECT_TRUE(m.commit_block(2));

  EXPECT_TRUE(
      m.add_child(2, 3, BlockExecutionMode::kBlockExecutionModeProposal));
  auto b3 = m.get_block_data(3);
  ASSERT_TRUE(b3);
  b3->close_block();
  EXPECT_TRUE(m.commit_block(3));

  EXPECT_FALSE(m.get_block_data(0));
  EXPECT_FALSE(m.get_block_data(1));
  EXPECT_FALSE(m.get_block_data(2));
  EXPECT_TRUE(m.get_block_data(3));
}

TEST(BlockManagerTest, AccessingSnapshotData)
{
  // edge case found from a runtime test
  InMemoryDatabase db;
  db.write_deployment(ContractAddress{},
                      std::make_shared<const Contract>(ContractType::kNative));
  BlockManager m(0, &db);
  m.set_cost_model(StorageCostModel::default_cost_model());

  EXPECT_TRUE(
      m.add_child(0, 1, BlockExecutionMode::kBlockExecutionModeProposal));
  ASSERT_TRUE(m.get_block_data(1));
  m.get_block_data(1)->close_block();
  EXPECT_TRUE(m.commit_block(1));

  EXPECT_TRUE(
      m.add_child(1, 2, BlockExecutionMode::kBlockExecutionModeProposal));
  ASSERT_TRUE(m.get_block_data(2));
  m.get_block_data(2)->close_block();
  EXPECT_TRUE(m.commit_block(2));

  EXPECT_TRUE(
      m.add_child(2, 3, BlockExecutionMode::kBlockExecutionModeProposal));
  ASSERT_TRUE(m.get_block_data(3));

  EXPECT_TRUE(m.get_block_data(3)->get_snapshot_view()->get_snapshot_deployment(
      ContractAddress{}));
}

namespace {

// A genesis carrying `network_config`, otherwise the smallest configuration
// the prerequisite loader accepts.
GenesisConfiguration
make_genesis(NetworkConfig network_config)
{
  Contract root(ContractType::kRv32);
  root.rv32_contract().data.push_back(0);
  return GenesisConfiguration{
      .storage_cost_model = StorageCostModel::default_cost_model().to_xdr(),
      .network_config = std::move(network_config),
      .root_contract = std::move(root),
      .default_wallet = native_default_wallet_hash(),
  };
}

} // namespace

// Genesis decides the chain's execution parameters, so what it carries is what
// the manager hands to the executor -- not whatever this build's stock values
// happen to be.
TEST(BlockManagerTest, GenesisInstallsItsOwnNetworkConfig)
{
  constexpr uint32_t kConfiguredDepth = 64;
  NetworkConfig configured = stock_network_config();
  NetworkConfigParams::set_param(configured,
                                 ConfigParamID::kConfigParamMaxCrossInvokeDepth,
                                 kConfiguredDepth);
  ASSERT_NE(
      NetworkConfigParams(configured.v0()).max_cross_invoke_depth,
      NetworkConfigParams(stock_network_config().v0()).max_cross_invoke_depth);

  InMemoryDatabase db;
  BlockManager m(0, &db);
  ASSERT_TRUE(m.initialize_genesis_configuration(make_genesis(configured)));

  EXPECT_EQ(m.get_network_config(), configured);
}

} // namespace hogd

int
main(int argc, char *argv[])
{
  auto ret = hogd::hogd_init(argc, argv, false);
  if (ret != 0)
    return ret;
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
