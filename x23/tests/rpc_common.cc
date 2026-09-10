#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <format>
#include <memory>
#include <string>
#include <type_traits>
#include <utility>

#include <sys/socket.h>
#include <unistd.h>

#include <gtest/gtest.h>

#include "tests/xdrtest.hh"
#include "x23/fileio.h"
#include "x23/rpc_common.h"

using namespace x23;

namespace {

using EchoMeta = blank_rpc_v1::proc_meta<1>;

class RpcLog : public ::testing::Test {
protected:
  UniqueFd fd_;

  void SetUp() override
  {
    char name[] = "rpc-log-XXXXXX";
    fd_.reset(mkstemp(name));
    ASSERT_TRUE(fd_);
    ASSERT_EQ(unlink(name), 0);
  }

  std::string contents()
  {
    EXPECT_EQ(lseek(fd_.get(), 0, SEEK_SET), 0);
    std::string ret;
    EXPECT_TRUE(readall(fd_, &ret));
    return ret;
  }
};

} // namespace

TEST(RpcMessage, CallAndRecord)
{
  auto msg = call_msg<EchoMeta>();
  msg.xid = 123;
  EXPECT_EQ(msg.body.mtype(), msg_type::CALL);
  EXPECT_EQ(msg.body.cbody().rpcvers, 2);
  EXPECT_EQ(msg.body.cbody().prog, EchoMeta::version_meta::program);
  EXPECT_EQ(msg.body.cbody().vers, EchoMeta::version_meta::version);
  EXPECT_EQ(msg.body.cbody().proc, EchoMeta::proc);
  EXPECT_EQ(msg.body.cbody().cred.flavor, auth_flavor::AUTH_NONE);
  EXPECT_TRUE(msg.body.cbody().cred.body.empty());
  EXPECT_FALSE(rpc_is_success(msg));
  EXPECT_EQ(msg_show_stat(msg), "CALL");

  Data payload{1, 2, 3};
  auto record = make_record(msg, payload);
  EXPECT_EQ(getbe<uint32_t>(record.data()), 0x80000000U | (record.size() - 4));
  auto expected = serialize(msg, payload);
  EXPECT_TRUE(std::ranges::equal(record.subspan(4), expected));
}

TEST(RpcMessage, AcceptedStatuses)
{
  for (auto status : {accept_stat::SUCCESS, accept_stat::PROG_UNAVAIL,
                      accept_stat::PROC_UNAVAIL, accept_stat::GARBAGE_ARGS,
                      accept_stat::SYSTEM_ERR}) {
    auto msg = call_msg<EchoMeta>();
    msg.xid = 17;
    msg_set_reply_stat(&msg, status);
    EXPECT_EQ(msg.xid, 17);
    EXPECT_EQ(msg.body.mtype(), msg_type::REPLY);
    EXPECT_EQ(msg.body.rbody().stat(), reply_stat::MSG_ACCEPTED);
    EXPECT_EQ(msg.body.rbody().areply().reply_data.stat(), status);
    EXPECT_EQ(rpc_is_success(msg), status == accept_stat::SUCCESS);
    EXPECT_EQ(msg_show_stat(msg), std::format("{}", status));
  }
}

TEST(RpcMessage, RejectedStatuses)
{
  auto msg = call_msg<EchoMeta>();
  msg_set_reply_stat(&msg, reject_stat::RPC_MISMATCH);
  EXPECT_EQ(msg.body.rbody().stat(), reply_stat::MSG_DENIED);
  EXPECT_EQ(msg.body.rbody().rreply().mismatch_info().low, 2);
  EXPECT_EQ(msg.body.rbody().rreply().mismatch_info().high, 2);
  EXPECT_FALSE(rpc_is_success(msg));
  EXPECT_EQ(msg_show_stat(msg), "RPC_MISMATCH");

  msg_set_reply_stat(&msg, auth_stat::AUTH_TOOWEAK);
  EXPECT_EQ(msg.body.rbody().rreply().stat(), reject_stat::AUTH_ERROR);
  EXPECT_EQ(msg.body.rbody().rreply().rj_why(), auth_stat::AUTH_TOOWEAK);
  EXPECT_FALSE(rpc_is_success(msg));
  EXPECT_EQ(msg_show_stat(msg), "AUTH_TOOWEAK");
  XdrCallFailed error(std::move(msg), "echo");
  EXPECT_STREQ(error.what(), "echo: RPC call rejected (AUTH_ERROR)");
  EXPECT_EQ(error.m_.body.rbody().rreply().rj_why(), auth_stat::AUTH_TOOWEAK);
}

TEST(RpcMessage, VersionMismatchDetails)
{
  auto msg = call_msg<EchoMeta>();
  msg_set_reply_stat(&msg, accept_stat::PROG_MISMATCH);
  auto &range = msg.body.rbody().areply().reply_data.mismatch_info();
  range.low = 2;
  range.high = 9;
  EXPECT_EQ(msg_show_stat(msg), "PROG_MISMATCH[low=2,high=9]");
  XdrCallFailed error(msg);
  EXPECT_STREQ(error.what(), "RPC call failed (PROG_MISMATCH)");
  EXPECT_EQ(error.m_, msg);
}

TEST(RpcServiceMap, ExactAndMissingVersions)
{
  SrvMap<int> services;
  services[{10, 2}] = std::make_unique<int>(12);
  services[{10, 9}] = std::make_unique<int>(19);
  services[{11, 100}] = std::make_unique<int>(1100);
  auto msg = call_msg<EchoMeta>();
  msg.xid = 456;
  msg.body.cbody().prog = 10;
  msg.body.cbody().vers = 2;
  rpc_msg error;
  EXPECT_EQ(services.lookup(msg, &error), services.at({10, 2}).get());
  EXPECT_EQ(error.xid, 456);

  for (uint32_t version : {0U, 1U, 3U, 10U, UINT32_MAX}) {
    msg.body.cbody().vers = version;
    EXPECT_EQ(services.lookup(msg, &error), nullptr);
    EXPECT_EQ(error.xid, 456);
    EXPECT_EQ(msg_show_stat(error), "PROG_MISMATCH[low=2,high=9]");
    EXPECT_EQ(msg.body.mtype(), msg_type::CALL);
  }
  EXPECT_EQ(services.lookup(&msg), nullptr);
  EXPECT_EQ(msg_show_stat(msg), "PROG_MISMATCH[low=2,high=9]");
}

TEST(RpcServiceMap, UnavailableProgramAndRpcVersion)
{
  SrvMap<int> services;
  auto msg = call_msg<EchoMeta>();
  msg.xid = 456;
  rpc_msg error;
  EXPECT_EQ(services.lookup(msg, &error), nullptr);
  EXPECT_EQ(msg_show_stat(error), "PROG_UNAVAIL");
  services[{10, 1}] = std::make_unique<int>(0);
  for (uint32_t program : {0U, 9U, 11U, UINT32_MAX}) {
    msg.body.cbody().prog = program;
    EXPECT_EQ(services.lookup(msg, &error), nullptr);
    EXPECT_EQ(msg_show_stat(error), "PROG_UNAVAIL");
  }
  msg.body.cbody().rpcvers = 3;
  EXPECT_EQ(services.lookup(&msg), nullptr);
  EXPECT_EQ(msg.xid, 456);
  EXPECT_EQ(msg_show_stat(msg), "RPC_MISMATCH");
}

TEST(RpcProcedure, DispatchesKnownAndUnknownNumbers)
{
  auto proc = []<typename Meta> {
    if constexpr (std::is_void_v<Meta>)
      return std::string("unknown");
    else
      return std::string(Meta::proc_name);
  };
  EXPECT_EQ((with_proc_meta<blank_rpc_v1, std::string>(0, proc)), "null");
  EXPECT_EQ((with_proc_meta<blank_rpc_v1, std::string>(1, proc)), "echo");
  EXPECT_EQ((with_proc_meta<blank_rpc_v1, std::string>(UINT32_MAX, proc)),
            "unknown");
}

TEST_F(RpcLog, TextMessagesAndDisabledLogger)
{
  auto msg = call_msg<EchoMeta>();
  msg.xid = 123;
  RpcLogger disabled("peer", -1);
  EXPECT_FALSE(disabled.active());
  disabled.log_client(msg);
  disabled.log_server(msg);
  disabled.log_client_fail(msg.xid, "disconnected");
  disabled.log_message("ignored {}", 42);
  EXPECT_TRUE(contents().empty());

  RpcLogger logger("peer", fd_.get());
  logger.log_client<EchoMeta>(msg);
  logger.log_server<blank_rpc_v1>(msg);
  logger.log_client_fail(msg.xid, "disconnected");
  logger.log_message("message {}", 42);
  msg_set_reply_stat(&msg, accept_stat::PROC_UNAVAIL);
  logger.log_server(msg);
  auto text = contents();
  EXPECT_TRUE(text.contains("peer <== xid=123 CALL blank_rpc_v1::echo\n"));
  EXPECT_TRUE(text.contains("peer ==> xid=123 CALL blank_rpc_v1::<proc#1>\n"));
  EXPECT_TRUE(text.contains("peer CALL xid=123 ==> disconnected\n"));
  EXPECT_TRUE(text.contains("peer message 42\n"));
  EXPECT_TRUE(text.contains("peer <== xid=123 REPLY FAIL PROC_UNAVAIL\n"));
}

TEST_F(RpcLog, DisabledCsvIgnoresAllMessages)
{
  RpcCsvLogger logger;
  EXPECT_FALSE(logger.active());
  auto msg = call_msg<EchoMeta>();
  logger.log_client(msg);
  logger.log_server(msg);
  logger.log_message("ignored {}", 42);
  logger.log_client_fail(msg.xid, "disconnected");
}

TEST_F(RpcLog, CsvOverLoopback)
{
  auto addr = HostPort("127.0.0.1", 0).to_addr().value();
  auto listener = stcplisten(addr);
  auto client = tcpconnect(local_addr(listener)).value();
  UniqueFd server(accept(listener.get(), nullptr, nullptr));
  ASSERT_TRUE(server);
  RpcCsvLogger logger(client.get(), fd_.get());
  EXPECT_TRUE(logger.active());
  auto msg = call_msg<EchoMeta>();
  msg.xid = 123;
  logger.log_client<EchoMeta>(msg);
  msg_set_reply_stat(&msg, accept_stat::SUCCESS);
  logger.log_server(msg);
  logger.log_client_fail(456, "disconnected");
  auto text = contents();
  EXPECT_TRUE(text.contains(",CLIENT,CALL,123,"));
  EXPECT_TRUE(text.contains(",SERVER,REPLY,123,"));
  EXPECT_TRUE(text.contains(",CLIENT,REPLY,456,"));
  EXPECT_TRUE(text.contains("blank_rpc_v1::echo\n"));
  EXPECT_TRUE(text.contains("/127.0.0.1:"));
  EXPECT_EQ(std::ranges::count(text, '\n'), 3);
}

TEST_F(RpcLog, PeerLookupFailure)
{
  RpcLogger logger(-1, fd_.get());
  EXPECT_TRUE(logger.peername_.contains("peer lookup failed"));
  logger.log_message("still usable");
  EXPECT_TRUE(contents().contains("still usable\n"));
  RpcCsvLogger disabled(-1, -1);
  EXPECT_FALSE(disabled.active());
}
