#include <format>
#include <gtest/gtest.h>
#include <stdexcept>

#include "coord.hh"
#include "exec_coord.hh"
#include "executor_to_db_rpc.hh"
#include "hogd/hogexec/interface/exec_coord_server.h"
#include "hogd/hogexec/runtime/native_contract_test_framework.h"
#include "hogd/init.h"
#include "x23/event.h"
#include "x23/srpc.h"
#include "x23/vec.h"


namespace hogd {

TEST(ExecCoordTest, TestConnection) {

    x23::RpcLogger::default_logfd = 1;

    MockDBWrapper mock_db;
    auto address = x23::uformat("localhost:{}", mock_db.exec_port());
    ExecCoordServer exec_coord_server(ExecCard{.protocol = "blah", .upid = {}, .address = address}, 1);

    exec_coord_server.init_from_coord(nullptr);
    
    auto port = exec_coord_server.start_server(0);
    ASSERT_NE(port, 0);

    x23::ThreadPool pool(4);
    x23::CoroPoll cp(pool);

    auto server_addr =
    x23::HostPort{"localhost",
                  x23::uformat("{}", port)}
        .to_addr(true);
    if (!server_addr) {
    throw std::runtime_error(
        std::format("failed server error: {}", server_addr.error()));
    }
    auto client =  x23::SRpcMulticlient<x23::RpcLogger>::make(server_addr.value())
        ->client(ExecutorCoord1{});

    client.executorcoord_null();
}

} // namespace hogd

int
main(int argc, char *argv[])
{
  auto ret = hogd::hogd_init(argc, argv);
  if (ret != 0)
    return ret;
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
