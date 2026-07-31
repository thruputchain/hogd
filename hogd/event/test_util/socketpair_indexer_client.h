#pragma once

#include <sys/socket.h>

#include "hogd/base/coropoll.h"
#include "hogd/base/log.h"
#include "hogd/event/event_block_context.h"
#include "indexer.hh"
#include "x23/rpc.h"

namespace hogd {

// Test helper: wires [server] (any IndexerEvent1 implementer) to a fresh
// socketpair and returns a connected client. Shared by the event-server tests
// that exercise indexer fan-out, so the RPC-stream setup lives in one place.
template<typename Server>
indexer_event_client_t
make_socketpair_indexer_client(Server *server)
{
  int fds[2];
  CHECK_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);
  x23::rpc_stream_logged(default_coropoll(), fds[1], -1)->server(server);
  auto client_stream = x23::rpc_stream_logged(default_coropoll(), fds[0], -1);
  return client_stream->client<IndexerEvent1>();
}

} // namespace hogd
