#include <gtest/gtest.h>

#include <sys/socket.h>

#include "tests/xdrtest.hh"
#include "x23/async.h"
#include "x23/cleanup.h"
#include "x23/rpc.h"
#include "x23/sockio.h"
#include "x23/srpc.h"
#include "x23/threadpool.h"

namespace x23 {

struct SyncServer {
  using rpc_service = blank_rpc_v1;
  void null() {}
  Data echo(Data arg) { return arg; }
};
static_assert(is_blank_rpc_v1<SyncServer *, std::type_identity_t>);

struct AsyncServer {
  using rpc_service = blank_rpc_v1;
  static Async<void> null() { co_return; }
  static Async<Data> echo(Data arg) { co_return arg; }
};
static_assert(is_blank_rpc_v1<AsyncServer *, Async>);

TEST(MaxMessageSize, SrpcClientRejectsOversizedMessage)
{
  SyncServer server;

  UniqueFd listener = stcplisten(0);
  ASSERT_TRUE(listener);
  uint16_t port = get_port(local_addr(listener));
  UniqueFd client_fd = tcpconnect("127.0.0.1", port);
  ASSERT_TRUE(client_fd);
  UniqueFd server_fd(::accept(listener.get(), nullptr, nullptr));
  ASSERT_TRUE(server_fd);

  srpc_stream(server_fd.release())->server(&server);

  RefPtr<SRpcStream<>> client_stream{*new SRpcStream<>(std::move(client_fd))};
  client_stream->max_message_size_ = 100;
  client_stream->go();
  auto client = client_stream->client(blank_rpc_v1{});

  Data big(200, 0xaa);
  EXPECT_THROW(client.echo(big), XdrBadInput);

  Data small{1, 2, 3};
  EXPECT_EQ(client.echo(small), small);
}

TEST(MaxMessageSize, AsyncRpcClientRejectsOversizedMessage)
{
  AsyncServer server;
  ThreadPool pool(2);
  CoroPoll cp(pool);

  int fds[2];
  ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);

  rpc_stream(cp, fds[1])->server(&server);

  auto cc = rpc_stream(cp, fds[0]);
  cc->max_message_size = 100;
  auto client = cc->client<blank_rpc_v1>();

  Data big(200, 0xaa);
  EXPECT_THROW(client.echo(big)(), XdrBadInput);

  Data small{1, 2, 3};
  EXPECT_EQ(client.echo(small)(), small);
}

// A single ONC RPC record marker leaves 31 bits for the payload length, so a
// message-size cap above kMaxRecordPayloadSize cannot be framed in one
// fragment.  Creating a client with such a cap must fail loudly at setup
// rather than let make_record() later truncate the marker and desync the
// stream (issue #3275).
TEST(MaxMessageSize, ClientCreationRejectsCapAboveRecordLimit)
{
  ThreadPool pool(2);
  CoroPoll cp(pool);

  int fds[2];
  ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);
  UniqueFd server_fd{fds[1]};

  auto cc = rpc_stream(cp, fds[0]);
  cc->max_message_size = kMaxRecordPayloadSize + 1;
  EXPECT_THROW(cc->client<blank_rpc_v1>(), XdrBadInput);
}

} // namespace x23
