// Tests for how RpcStream's reader frames incoming bytes: several
// records per read, records split across reads, records larger than
// the reader's buffer, multi-fragment records, and malformed input.

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <gtest/gtest.h>

#include "tests/xdrtest.hh"
#include "x23/fileio.h"
#include "x23/rpc.h"

using namespace x23;

namespace {

using EchoMeta = blank_rpc_v1::proc_meta<1>;

struct EchoServer {
  using rpc_service = blank_rpc_v1;
  void null() {}
  Data echo(Data arg) { return arg; }
};

// One end of a socketpair is handed to an RpcStream, while the test
// reads and writes raw bytes on the other.
struct Pair {
  int stream_fd;
  UniqueFd peer;

  Pair()
  {
    int fds[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds))
      Errno{}.raise("socketpair");
    stream_fd = fds[0];
    peer.reset(fds[1]);
    // Fail rather than hang if the stream never answers.
    timeval tv{.tv_sec = 10, .tv_usec = 0};
    setsockopt(peer.get(), SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  }
};

Data
pattern(size_t size, uint32_t seed)
{
  Data ret;
  ret.reserve(size);
  for (size_t i = 0; i < size; ++i)
    ret.push_back(uint8_t(seed * 31 + i * 7));
  return ret;
}

Buf
echo_call(uint32_t xid, const Data &data)
{
  auto msg = call_msg<EchoMeta>();
  msg.xid = xid;
  return make_record(msg, data);
}

Buf
echo_reply(uint32_t xid, const Data &data)
{
  rpc_msg msg{.xid = xid};
  msg_set_reply_stat(&msg, accept_stat::SUCCESS);
  return make_record(msg, data);
}

void
append(Buf *out, BufView bytes)
{
  out->append(bytes.begin(), bytes.end());
}

// Re-frame a single-fragment record as several fragments of at most
// chunk bytes.
Buf
fragment(BufView record, size_t chunk)
{
  Buf ret;
  BufView body = record.subspan(4);
  while (!body.empty()) {
    size_t n = std::min(chunk, body.size());
    uint8_t marker[4];
    putbe<uint32_t>(marker,
                    uint32_t(n) | (n == body.size() ? kLastFragment : 0));
    append(&ret, BufView{marker, 4});
    append(&ret, body.first(n));
    body = body.subspan(n);
  }
  return ret;
}

// Read one record from the stream and return it without its marker.
Buf
read_record(BorrowedFd peer)
{
  Buf marker, record;
  if (auto res = fullread(peer, &marker, 4); !res)
    res.error().raise("read record marker");
  auto length = getbe<uint32_t>(marker.data());
  EXPECT_NE(length & kLastFragment, 0U);
  if (auto res = fullread(peer, &record, length & ~kLastFragment); !res)
    res.error().raise("read record");
  return record;
}

// Read a successful reply to an echo call, returning the xid and data.
std::pair<uint32_t, Data>
read_echo_reply(BorrowedFd peer)
{
  Buf record = read_record(peer);
  rpc_msg msg;
  size_t n = deserialize_from(&msg, record);
  EXPECT_TRUE(rpc_is_success(msg));
  Data data;
  BufView body = BufView(record).subspan(n);
  deserialize(&data, body);
  return {msg.xid, std::move(data)};
}

uint32_t
read_call_xid(BorrowedFd peer)
{
  Buf record = read_record(peer);
  rpc_msg msg;
  deserialize_from(&msg, record);
  EXPECT_EQ(msg.body.mtype(), msg_type::CALL);
  return msg.xid;
}

// True if the stream closes its end of the socket without sending
// anything more.
bool
closed_by_stream(BorrowedFd peer)
{
  pollfd pfd{.fd = peer.get(), .events = POLLIN, .revents = 0};
  if (poll(&pfd, 1, 10'000) != 1)
    return false;
  // Linux reports ECONNRESET instead of EOF if the stream closed the
  // socket without having read everything we sent.
  char c;
  ssize_t n = read(peer.get(), &c, 1);
  return n == 0 || (n == -1 && errno == ECONNRESET);
}

// The contents of the bytes a RecvBuf has received but not consumed.
Data
pending_bytes(const detail::RecvBuf &buf)
{
  return Data(buf.rptr(), buf.rptr() + buf.avail());
}

// Receive size bytes of pattern(size, seed), as the reader would.
void
receive(detail::RecvBuf *buf, size_t size, uint32_t seed)
{
  Data bytes = pattern(size, seed);
  ASSERT_GE(buf->space(), size);
  memcpy(buf->wptr(), bytes.data(), size);
  buf->commit(size);
}

TEST(RecvBuf, ReusesChunkNobodyReferences)
{
  detail::RecvBuf buf;
  buf.make_room(4);
  const detail::RecvChunk *chunk = buf.chunk().get();
  const uint8_t *start = buf.wptr();
  EXPECT_EQ(buf.space(), detail::RecvBuf::kChunkSize);

  // Consuming everything lets the next read start over at the front.
  for (uint32_t i = 0; i < 100; ++i) {
    receive(&buf, 1000, i);
    buf.consume(1000);
    buf.make_room(4);
    EXPECT_EQ(buf.chunk().get(), chunk);
    EXPECT_EQ(buf.wptr(), start);
  }

  // A partial record at the end of the chunk moves to the front once
  // there is too little room left to finish it.
  receive(&buf, buf.space(), 7);
  buf.consume(detail::RecvBuf::kChunkSize - 10);
  Data partial = pending_bytes(buf);
  ASSERT_EQ(partial.size(), 10U);
  buf.make_room(90);
  EXPECT_EQ(buf.chunk().get(), chunk);
  EXPECT_EQ(buf.rptr(), start);
  EXPECT_EQ(pending_bytes(buf), partial);
  EXPECT_GE(buf.space(), 90U);
}

TEST(RecvBuf, PreservesBytesStillReferenced)
{
  detail::RecvBuf buf;
  buf.make_room(4);

  // Simulate a reply that a client has not finished with.
  receive(&buf, 500, 1);
  RefPtr<detail::RecvChunk> held = buf.chunk();
  const uint8_t *reply = buf.rptr();
  buf.consume(500);

  // While there is room, keep filling the same chunk past the reply
  // instead of starting over at the front.
  buf.make_room(4);
  EXPECT_EQ(buf.chunk().get(), held.get());
  EXPECT_EQ(buf.wptr(), reply + 500);

  // When there is not, move the partial record to a new chunk and
  // leave the old one alone.
  receive(&buf, buf.space(), 2);
  buf.consume(buf.avail() - 10);
  Data partial = pending_bytes(buf);
  buf.make_room(90);
  EXPECT_NE(buf.chunk().get(), held.get());
  EXPECT_EQ(pending_bytes(buf), partial);
  EXPECT_GE(buf.space(), 90U);
  EXPECT_EQ(Data(reply, reply + 500), pattern(500, 1));

  // The new chunk is unreferenced, so gets reused in place.
  const detail::RecvChunk *fresh = buf.chunk().get();
  buf.consume(buf.avail());
  buf.make_room(4);
  EXPECT_EQ(buf.chunk().get(), fresh);
  EXPECT_EQ(buf.wptr(), buf.chunk()->data());
}

TEST(RecvBuf, GrowsForLargeRecordThenShrinks)
{
  using detail::RecvBuf;
  RecvBuf buf;
  buf.make_room(4);
  receive(&buf, 100, 3);
  Data partial = pending_bytes(buf);

  // A record that cannot fit in a chunk gets one big enough to hold
  // it contiguously, without losing the part already received.
  const size_t big = 2 * RecvBuf::kMaxRetain;
  buf.make_room(big - 100);
  EXPECT_EQ(pending_bytes(buf), partial);
  EXPECT_GE(buf.space(), big - 100);
  EXPECT_GE(buf.chunk()->capacity(), big);

  // The oversized chunk is released once drained.
  buf.commit(big - 100);
  buf.consume(big);
  buf.make_room(4);
  EXPECT_EQ(buf.chunk()->capacity(), RecvBuf::kChunkSize);

  // A moderately large chunk, on the other hand, is kept, so that a
  // stream of such records does not allocate for each of them.
  const size_t medium = 3 * RecvBuf::kChunkSize;
  buf.make_room(medium);
  const detail::RecvChunk *chunk = buf.chunk().get();
  EXPECT_GE(chunk->capacity(), medium);
  buf.commit(medium);
  buf.consume(medium);
  buf.make_room(4);
  EXPECT_EQ(buf.chunk().get(), chunk);
}

struct ReaderTest : testing::Test {
  ThreadPool pool_{4};
  CoroPoll cp_{pool_};
  EchoServer server_;
};

TEST_F(ReaderTest, ManyCallsInOneWrite)
{
  Pair p;
  rpc_stream(cp_, p.stream_fd)->server(&server_);

  constexpr uint32_t kCalls = 500;
  Buf all;
  for (uint32_t i = 0; i < kCalls; ++i)
    append(&all, echo_call(i + 1, pattern(i % 37, i)));
  ASSERT_TRUE(fullwrite(p.peer, all));

  for (uint32_t i = 0; i < kCalls; ++i) {
    auto [xid, data] = read_echo_reply(p.peer);
    EXPECT_EQ(xid, i + 1);
    EXPECT_EQ(data, pattern(i % 37, i));
  }
}

TEST_F(ReaderTest, CallsSplitAcrossReads)
{
  Pair p;
  rpc_stream(cp_, p.stream_fd)->server(&server_);

  Buf all;
  for (uint32_t i = 0; i < 4; ++i)
    append(&all, echo_call(i + 1, pattern(10 + i, i)));
  // Every prefix of the stream, including a partial record marker,
  // gets its own write.
  for (uint8_t byte : all) {
    ASSERT_EQ(write(p.peer.get(), &byte, 1), 1);
    std::this_thread::yield();
  }

  for (uint32_t i = 0; i < 4; ++i) {
    auto [xid, data] = read_echo_reply(p.peer);
    EXPECT_EQ(xid, i + 1);
    EXPECT_EQ(data, pattern(10 + i, i));
  }
}

TEST_F(ReaderTest, RecordsLargerThanBuffer)
{
  Pair p;
  rpc_stream(cp_, p.stream_fd)->server(&server_);

  // Alternate records far larger than the reader's 64KiB chunk with
  // small ones, so the reader has to grow, shrink, and compact.
  const size_t sizes[] = {300'000, 5, 70'000, 2'000'000, 0, 65'000, 300'000};
  std::thread writer([&] {
    uint32_t xid = 0;
    for (size_t size : sizes) {
      ++xid;
      EXPECT_TRUE(fullwrite(p.peer, echo_call(xid, pattern(size, xid))));
    }
  });
  uint32_t expect = 0;
  for (size_t size : sizes) {
    auto [xid, data] = read_echo_reply(p.peer);
    EXPECT_EQ(xid, ++expect);
    EXPECT_EQ(data, pattern(size, expect));
  }
  writer.join();
}

TEST_F(ReaderTest, FragmentedCall)
{
  Pair p;
  rpc_stream(cp_, p.stream_fd)->server(&server_);

  Buf all = fragment(echo_call(1, pattern(100, 1)), 17);
  append(&all, echo_call(2, pattern(3, 2)));
  append(&all, fragment(echo_call(3, pattern(50, 3)), 1));
  ASSERT_TRUE(fullwrite(p.peer, all));

  const size_t sizes[] = {100, 3, 50};
  for (uint32_t i = 0; i < 3; ++i) {
    auto [xid, data] = read_echo_reply(p.peer);
    EXPECT_EQ(xid, i + 1);
    EXPECT_EQ(data, pattern(sizes[i], i + 1));
  }
}

TEST_F(ReaderTest, OversizedRecordFailsStream)
{
  for (bool fragmented : {false, true}) {
    Pair p;
    {
      auto conf = rpc_stream(cp_, p.stream_fd);
      conf->max_message_size = 100;
      conf->server(&server_);
    }

    // A record within the limit still works.
    ASSERT_TRUE(fullwrite(p.peer, echo_call(1, pattern(20, 1))));
    EXPECT_EQ(read_echo_reply(p.peer).second, pattern(20, 1));

    Buf big = echo_call(2, pattern(200, 2));
    if (fragmented)
      // No single fragment exceeds the limit, but their sum does.
      big = fragment(big, 60);
    ASSERT_TRUE(fullwrite(p.peer, big));
    EXPECT_TRUE(closed_by_stream(p.peer));
  }
}

TEST_F(ReaderTest, MalformedRecordFailsStream)
{
  enum class Bad { kShort, kMessageType, kCallHeader, kEofMidRecord };
  for (auto bad :
       {Bad::kShort, Bad::kMessageType, Bad::kCallHeader, Bad::kEofMidRecord}) {
    Pair p;
    rpc_stream(cp_, p.stream_fd)->server(&server_);

    Buf record = echo_call(1, pattern(8, 1));
    switch (bad) {
    case Bad::kShort:
      // Too short to hold even an xid and message type.
      record.resize(8);
      putbe<uint32_t>(record.data(), kLastFragment | 4);
      break;
    case Bad::kMessageType:
      putbe<uint32_t>(record.data() + 8, 7);
      break;
    case Bad::kCallHeader:
      // RPC version field of the call body
      record.resize(16);
      putbe<uint32_t>(record.data(), kLastFragment | 12);
      break;
    case Bad::kEofMidRecord:
      record.resize(record.size() - 2);
      break;
    }
    ASSERT_TRUE(fullwrite(p.peer, record));
    if (bad == Bad::kEofMidRecord)
      shutdown(p.peer.get(), SHUT_WR);
    EXPECT_TRUE(closed_by_stream(p.peer));
  }
}

// A stream must not lose bytes that an earlier CoroPoll::read() on the
// same file descriptor pulled into the descriptor's read buffer.
TEST_F(ReaderTest, PicksUpBytesAlreadyBuffered)
{
  Pair p;
  Buf all{uint8_t('!')};
  append(&all, echo_call(1, pattern(9, 1)));
  append(&all, echo_call(2, pattern(4000, 2)));
  ASSERT_TRUE(fullwrite(p.peer, all));

  set_nonblock(p.stream_fd);
  Buf prefix;
  auto res = cp_.read(p.stream_fd, &prefix, 1)();
  ASSERT_TRUE(res);
  ASSERT_EQ(prefix, Buf{uint8_t('!')});

  rpc_stream(cp_, p.stream_fd)->server(&server_);
  EXPECT_EQ(read_echo_reply(p.peer).second, pattern(9, 1));
  EXPECT_EQ(read_echo_reply(p.peer).second, pattern(4000, 2));
}

TEST_F(ReaderTest, ManyRepliesInOneWrite)
{
  Pair p;
  auto stream = rpc_stream(cp_, p.stream_fd);
  auto client = stream->client(blank_rpc_v1{});

  // About 300KiB of replies, so that they span several of the
  // reader's buffers and some straddle two reads.
  constexpr uint32_t kCalls = 300;
  using Pending = decltype(client.echo(Data{}));
  std::vector<Pending> pending;
  std::vector<uint32_t> xids;
  for (uint32_t i = 0; i < kCalls; ++i) {
    pending.push_back(client.echo(Data{}));
    xids.push_back(read_call_xid(p.peer));
  }

  // Answer in the opposite order from the calls.
  Buf all;
  for (uint32_t i = kCalls; i-- > 0;)
    append(&all, echo_reply(xids[i], pattern(700 + i, i)));
  ASSERT_TRUE(fullwrite(p.peer, all));

  for (uint32_t i = 0; i < kCalls; ++i)
    EXPECT_EQ(pending[i](), pattern(700 + i, i));
  stream->fail();
}

TEST_F(ReaderTest, FragmentedAndUnknownReplies)
{
  Pair p;
  auto stream = rpc_stream(cp_, p.stream_fd);
  auto client = stream->client(blank_rpc_v1{});

  auto pending = client.echo(Data{});
  uint32_t xid = read_call_xid(p.peer);
  // A reply nobody is waiting for is ignored.
  Buf all = echo_reply(xid + 1000, pattern(5, 5));
  append(&all, fragment(echo_reply(xid, pattern(77, 7)), 13));
  ASSERT_TRUE(fullwrite(p.peer, all));
  EXPECT_EQ(pending(), pattern(77, 7));
  stream->fail();
}

TEST_F(ReaderTest, GarbledReplyHeaderFailsStream)
{
  Pair p;
  auto stream = rpc_stream(cp_, p.stream_fd);
  auto client = stream->client(blank_rpc_v1{});

  auto first = client.echo(Data{});
  uint32_t xid = read_call_xid(p.peer);
  auto second = client.echo(Data{});
  read_call_xid(p.peer);

  // Valid xid and message type, but a nonsensical reply_stat.
  Buf reply = echo_reply(xid, pattern(4, 4));
  putbe<uint32_t>(reply.data() + 12, 99);
  ASSERT_TRUE(fullwrite(p.peer, reply));

  try {
    first();
    FAIL() << "garbled reply was accepted";
  } catch (const XdrException &e) {
    EXPECT_NE(std::string_view(e.what()).find("echo"), std::string_view::npos);
  }
  // The other call learns that the stream is gone.
  EXPECT_THROW(second(), XdrException);
}

} // namespace
