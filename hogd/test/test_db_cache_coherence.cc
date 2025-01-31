#include <gtest/gtest.h>

#include "hogd/hogdb/hogdb_core.h"
#include "rpc.hh"
#include "rpc_types.hh"

namespace hogd {

class TestDbCacheCoherence : public ::testing::Test {
protected:
  using coord_t = DatabaseToCoordInterfaceV1::client<
      x23::RpcStream<x23::RpcLogger>::ClientRef>;
  using exec_t = DatabaseToExecutorInterfaceV1::client<
      x23::RpcStream<x23::RpcLogger>::ClientRef>;

  const std::string kDBToExecutorPort = std::to_string(kDBToExecutorPortNum);
  const std::string kDBToCoordPort = std::to_string(kDBToCoordPortNum);

  exec_t connect_from_exec()
  {
    auto fd = cp_->tcpconnect(std::string("localhost"),
                              std::string(kDBToExecutorPort))();
    assert(fd);
    return x23::rpc_stream_logged(*cp_, *fd)
        .template client<DatabaseToExecutorInterfaceV1>();
  }

  coord_t connect_from_coord()
  {
    auto fd = cp_->tcpconnect(std::string("localhost"),
                              std::string(kDBToCoordPort))();
    assert(fd);
    return x23::rpc_stream_logged(*cp_, *fd)
        .template client<DatabaseToCoordInterfaceV1>();
  }

  void SetUp() override
  {
    pool_ = std::make_unique<x23::ThreadPool>(1);
    cp_ = std::make_unique<x23::CoroPoll>(*pool_);

    db_core_ =
        std::make_unique<HogdbCore>(1, kDBToExecutorPort, kDBToCoordPort);

    coord_client_ = std::make_unique<coord_t>(connect_from_coord());
    exec_client_ = std::make_unique<exec_t>(connect_from_exec());
  }

  std::unique_ptr<x23::ThreadPool> pool_;
  std::unique_ptr<x23::CoroPoll> cp_;

  std::unique_ptr<HogdbCore> db_core_;

  std::unique_ptr<coord_t> coord_client_;
  std::unique_ptr<exec_t> exec_client_;

  constexpr static RPCFlagSet kFlags =
      std::to_underlying(RPCFlags::kFinalCommit);
};

TEST_F(TestDbCacheCoherence, Basic)
{

  auto make_nnint_delta = [](int128_t set, int128_t add) {
    StorageDelta delta(StorageDeltaType::kNonnegativeIntSetAdd);
    delta.nnint().set_value = to_xdr(set);
    delta.nnint().add_value = to_xdr(add);
    return delta;
  };

  ASSERT_EQ(coord_client_->open_database_snapshot(0, 1)(), RPCError::kNone);
  ASSERT_EQ(coord_client_->open_database_snapshot(0, 2)(), RPCError::kNone);

  ASSERT_EQ(exec_client_->try_reserve_delta(1, 1, AddressAndKey{},
                                            make_nnint_delta(10, 5), kFlags)(),
            RPCError::kNone);

  ASSERT_EQ(exec_client_->try_reserve_delta(2, 2, AddressAndKey{},
                                            make_nnint_delta(10, 12), kFlags)(),
            RPCError::kNone);
  exec_client_->commit_transaction(1, 1)();

  ASSERT_EQ(exec_client_->get_item(1, AddressAndKey{})().error(),
            RPCError::kNoStorageItem);
  ASSERT_EQ(exec_client_->get_item(2, AddressAndKey{})().error(),
            RPCError::kNoStorageItem);
  exec_client_->commit_transaction(2, 2)();

  // Now database has two blocks (1 and 2), both of which are children of 0.

  coord_client_->close_snapshot(1)();
  coord_client_->close_snapshot(2)();

  ASSERT_EQ(coord_client_->open_database_snapshot(1, 11)(), RPCError::kNone);
  ASSERT_EQ(coord_client_->open_database_snapshot(1, 12)(), RPCError::kNone);
  ASSERT_EQ(coord_client_->open_database_snapshot(2, 21)(), RPCError::kNone);

  auto check_item = [this](BlockID block_id, int128_t expected_value) {
    auto item = exec_client_->get_item(block_id, AddressAndKey{})();
    ASSERT_EQ(item.error(), RPCError::kNone);
    ASSERT_EQ(item.item().type(), StorageItemType::kNonnegativeInt);
    ASSERT_EQ(item.item().nnint(), to_xdr(expected_value));
  };

  check_item(11, 15);
  check_item(12, 15);
  check_item(21, 22);

  ASSERT_EQ(exec_client_->try_reserve_delta(11, 11, AddressAndKey{},
                                            make_nnint_delta(100, -5),
                                            kFlags)(),
            RPCError::kNone);

  ASSERT_EQ(exec_client_->try_reserve_delta(12, 12, AddressAndKey{},
                                            make_nnint_delta(200, -5),
                                            kFlags)(),
            RPCError::kNone);

  exec_client_->commit_transaction(11, 11)();
  exec_client_->commit_transaction(12, 12)();

  check_item(11, 15);
  check_item(12, 15);
  check_item(21, 22);

  coord_client_->close_snapshot(11)();
  coord_client_->close_snapshot(12)();
  coord_client_->close_snapshot(21)();

  ASSERT_EQ(coord_client_->open_database_snapshot(11, 111)(), RPCError::kNone);

  check_item(111, 95);
  coord_client_->commit_snapshot(1)();
  check_item(111, 95);
  coord_client_->commit_snapshot(11)();
  check_item(111, 95);

  coord_client_->close_snapshot(111)();
}

TEST_F(TestDbCacheCoherence, ReadFromCommittedDB)
{

  auto make_nnint_delta = [](int128_t set, int128_t add) {
    StorageDelta delta(StorageDeltaType::kNonnegativeIntSetAdd);
    delta.nnint().set_value = to_xdr(set);
    delta.nnint().add_value = to_xdr(add);
    return delta;
  };
  auto check_item = [this](BlockID block_id, int128_t expected_value) {
    auto item = exec_client_->get_item(block_id, AddressAndKey{})();
    ASSERT_EQ(item.error(), RPCError::kNone);
    ASSERT_EQ(item.item().type(), StorageItemType::kNonnegativeInt);
    ASSERT_EQ(item.item().nnint(), to_xdr(expected_value));
  };

  ASSERT_EQ(coord_client_->open_database_snapshot(0, 1)(), RPCError::kNone);

  ASSERT_EQ(exec_client_->try_reserve_delta(1, 1, AddressAndKey{},
                                            make_nnint_delta(10, 5), kFlags)(),
            RPCError::kNone);

  exec_client_->commit_transaction(1, 1)();
  coord_client_->close_snapshot(1)();

  ASSERT_EQ(coord_client_->open_database_snapshot(1, 11)(), RPCError::kNone);
  check_item(11, 15);
  coord_client_->close_snapshot(11)();

  // a block that doesn't modify the item
  ASSERT_EQ(coord_client_->open_database_snapshot(11, 111)(), RPCError::kNone);
  coord_client_->close_snapshot(111)();

  coord_client_->commit_snapshot(1)();
  coord_client_->commit_snapshot(11)();

  ASSERT_EQ(coord_client_->open_database_snapshot(111, 1111)(),
            RPCError::kNone);

  // This forces the db to load the item from the committed db, not from a local
  // cache.
  check_item(1111, 15);
  coord_client_->close_snapshot(1111)();
}

} // namespace hogd
