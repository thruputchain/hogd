#pragma once

#include <gtest/gtest.h>

#include <sys/socket.h>

#include "hogd/hogexec/runtime/storage_snapshot_cache.h"
#include "hogd/hogexec/runtime/syscall_handler.h"
#include "hogd/hogexec/runtime/runtime_test_framework.h"
#include "hogd/hogexec/runtime/format.h"
#include "hogd/softstate/local_softstate_map.h"
#include "x23/rpc.h"
#include "x23/threadpool.h"
#include "x23/vec.h"
#include "storage_interface.hh"

namespace hogd :: test {

class RuntimeTestFramework {
public:

  x23::ThreadPool threadpool_ = x23::ThreadPool(2);
  x23::CoroPoll cp_ = x23::CoroPoll(threadpool_);

  using rpc_config_t = x23::RpcStream<x23::RpcLogger>::Configurator;

  int socket_fds_[2];

  std::unique_ptr<LocalContractMap> contract_map_;
  std::unique_ptr<LocalSoftstateMap> softstate_map_;
  std::unique_ptr<LocalSoftstateReadServer> read_storage_server_;
  std::unique_ptr<LocalSoftstateWriteServer> write_storage_server_;


  std::unique_ptr<StorageSnapshotCache> snapshot_cache_;

  void initialize() {
    contract_map_ = std::make_unique<LocalContractMap>();
    softstate_map_ = std::make_unique<LocalSoftstateMap>();
    read_storage_server_ =
        std::make_unique<LocalSoftstateReadServer>(*softstate_map_,
                                                   *contract_map_);
    write_storage_server_ =
        std::make_unique<LocalSoftstateWriteServer>(*softstate_map_,
                                                   *contract_map_);

    ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, socket_fds_), 0);

    rpc_config_t server_config = x23::rpc_stream_logged(cp_, socket_fds_[0]);
    rpc_config_t client_config = x23::rpc_stream_logged(cp_, socket_fds_[1]);

    snapshot_cache_ = std::make_unique<StorageSnapshotCache>(client_config);

    server_config.server(read_storage_server_.get());
    server_config.server(write_storage_server_.get());
  }

  static void check_log_eq(LogEntry const &entry,
                           x23::Vec<uint8_t> const &calldata)
  {
    ASSERT_EQ(entry.message.size(), calldata.size());
    int res =
        std::memcmp(entry.message.data(), calldata.data(), calldata.size());
    EXPECT_EQ(0, res);
    if (res != 0) {
      std::printf(
          std::format("detailed log eq check: {} {}\n", entry.message, calldata)
              .c_str());
    }
  }
};


}
