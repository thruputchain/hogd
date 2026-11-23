#include <array>
#include <cstdint>
#include <string_view>
#include <utility>

#include <fcntl.h>
#include <sys/socket.h>

#include <gtest/gtest.h>

#include "tests/xdrtest.hh"
#include "x23/fileio.h"
#include "x23/rpc.h"

using namespace x23;

namespace {

rpc_msg
read_call(BorrowedFd peer)
{
  Buf marker, record;
  auto res = fullread(peer, &marker, 4);
  if (!res)
    res.error().raise("read record marker");
  auto length = getbe<uint32_t>(marker.data());
  EXPECT_NE(length & 0x80000000U, 0U);
  res = fullread(peer, &record, length & 0x7fffffffU);
  if (!res)
    res.error().raise("read call");
  rpc_msg msg;
  deserialize_from(&msg, record);
  EXPECT_EQ(msg.body.mtype(), msg_type::CALL);
  return msg;
}

} // namespace

TEST(RpcClient, ReplyFailuresAndCopiedClientLifetime)
{
  enum class Reply { kSuccess, kRejected, kTruncated, kEof };
  ThreadPool pool(8);
  CoroPoll cp(pool);
  UniqueFd log(open("/dev/null", O_WRONLY | O_CLOEXEC));
  ASSERT_TRUE(log);
  for (auto mode :
       {Reply::kSuccess, Reply::kRejected, Reply::kTruncated, Reply::kEof}) {
    int fds[2];
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);
    UniqueFd peer(fds[1]);
    auto configurator = rpc_stream_logged(cp, fds[0], log.get());
    auto moved = std::move(configurator);
    auto client = moved->client(blank_rpc_v1{});
    auto copy = client;
    auto pending = copy.echo(Data{1, 2});
    auto call = read_call(peer);
    EXPECT_EQ(call.body.cbody().proc, 1U);
    if (mode == Reply::kEof)
      peer.reset();
    else {
      msg_set_reply_stat(&call, mode == Reply::kRejected
                                    ? accept_stat::SYSTEM_ERR
                                    : accept_stat::SUCCESS);
      Buf response;
      if (mode == Reply::kSuccess)
        response = make_record(call, Data{3, 4});
      else if (mode == Reply::kTruncated)
        response = make_record(call, uint32_t{8});
      else
        response = make_record(call);
      ASSERT_TRUE(fullwrite(peer, response));
    }
    if (mode == Reply::kSuccess)
      EXPECT_EQ(pending(), (Data{3, 4}));
    else if (mode == Reply::kRejected) {
      try {
        pending();
        FAIL() << "RPC error reply was accepted";
      } catch (const XdrCallFailed &error) {
        EXPECT_EQ(error.m_.body.rbody().areply().reply_data.stat(),
                  accept_stat::SYSTEM_ERR);
        EXPECT_NE(std::string_view(error.what()).find("echo"),
                  std::string_view::npos);
      }
    }
    else
      EXPECT_THROW(pending(), XdrException);
    moved->fail();
  }
}

TEST(RpcClient, VoidReplyRejectsUnexpectedBody)
{
  ThreadPool pool(8);
  CoroPoll cp(pool);
  for (bool malformed : {false, true}) {
    int fds[2];
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);
    UniqueFd peer(fds[1]);
    auto stream = rpc_stream(cp, fds[0]);
    auto client = stream->client(blank_rpc_v1{});
    auto pending = client.null();
    auto call = read_call(peer);
    msg_set_reply_stat(&call, accept_stat::SUCCESS);
    auto response =
        malformed ? make_record(call, uint32_t{1}) : make_record(call);
    ASSERT_TRUE(fullwrite(peer, response));
    if (malformed)
      EXPECT_THROW(pending(), XdrException);
    else
      EXPECT_NO_THROW(pending());
    stream->fail();
  }
}
