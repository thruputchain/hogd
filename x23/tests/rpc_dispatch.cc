#include <atomic>
#include <cstdint>
#include <stdexcept>
#include <tuple>
#include <utility>

#include <gtest/gtest.h>

#include "tests/xdrtest.hh"
#include "x23/rpc.h"
#include "x23/srpc.h"

using namespace x23;

namespace {

struct LogCounts {
  unsigned calls{}, replies{};
};

struct TestLogger {
  LogCounts *counts_;

  template<typename Meta, typename... Args>
  void log_server(const rpc_msg &msg, const Args &...) const
  {
    if (msg.body.mtype() == msg_type::CALL)
      ++counts_->calls;
    else
      ++counts_->replies;
  }
};

struct Context {
  unsigned calls{};
  bool fail{};

  void invoke()
  {
    ++calls;
    if (fail)
      throw std::runtime_error("handler failure");
  }
};

struct SyncServer {
  using rpc_service = blank_rpc_v1;

  void null(Context *context) { context->invoke(); }
  Data echo(Data data, Context *context)
  {
    context->invoke();
    data.push_back(42);
    return data;
  }
};

struct ReadyServer {
  using rpc_service = blank_rpc_v1;

  MaybeAsync<void> null(Context *context)
  {
    context->invoke();
    return {};
  }
  MaybeAsync<Data> echo(Data data, Context *context)
  {
    context->invoke();
    data.push_back(42);
    return data;
  }
};

struct SuspendedServer {
  using rpc_service = blank_rpc_v1;
  Awaitable<void> gate_;
  CoroCB<void> resume_{gate_};

  Async<void> null(Context *context)
  {
    co_await gate_;
    context->invoke();
  }
  Async<Data> echo(Data data, Context *context)
  {
    co_await gate_;
    context->invoke();
    data.push_back(42);
    co_return data;
  }
};

using NullMeta = blank_rpc_v1::proc_meta<0>;
using EchoMeta = blank_rpc_v1::proc_meta<1>;

BufView
check_reply(const Buf &record, uint32_t xid, accept_stat status)
{
  EXPECT_GE(record.size(), 4UZ);
  EXPECT_EQ(getbe<uint32_t>(record.data()),
            0x80000000U | uint32_t(record.size() - 4));
  rpc_msg msg;
  auto size = deserialize_from(&msg, BufView(record).subspan(4));
  EXPECT_EQ(msg.xid, xid);
  EXPECT_EQ(msg.body.mtype(), msg_type::REPLY);
  EXPECT_EQ(msg.body.rbody().stat(), reply_stat::MSG_ACCEPTED);
  EXPECT_EQ(msg.body.rbody().areply().reply_data.stat(), status);
  return BufView(record).subspan(size + 4);
}

template<typename Dispatcher>
void
check_dispatch(Dispatcher &dispatcher, Context &context, LogCounts &logs)
{
  EXPECT_EQ(dispatcher.getpv(), (ProgVers{blank_rpc_v1::program, 1}));
  auto call = call_msg<EchoMeta>();
  call.xid = 123;
  auto args = serialize(Data{1, 2, 3});
  auto serve = [&](BufView bytes) {
    auto result = dispatcher.serve(call, bytes);
    if constexpr (is_awaitable<decltype(result)>)
      return result();
    else
      return result;
  };
  auto reply = serve(args);
  Data out;
  deserialize(&out, check_reply(reply, call.xid, accept_stat::SUCCESS));
  EXPECT_EQ(out, (Data{1, 2, 3, 42}));
  EXPECT_EQ(context.calls, 1U);

  call = call_msg<NullMeta>();
  call.xid = 124;
  reply = serve({});
  EXPECT_TRUE(check_reply(reply, call.xid, accept_stat::SUCCESS).empty());
  EXPECT_EQ(context.calls, 2U);

  call.body.cbody().proc = 1000;
  reply = serve({});
  EXPECT_TRUE(check_reply(reply, call.xid, accept_stat::PROC_UNAVAIL).empty());
  EXPECT_EQ(context.calls, 2U);

  call = call_msg<EchoMeta>();
  call.xid = 125;
  reply = serve(BufView(args).first(4));
  EXPECT_TRUE(check_reply(reply, call.xid, accept_stat::GARBAGE_ARGS).empty());
  EXPECT_EQ(context.calls, 2U);

  context.fail = true;
  reply = serve(args);
  EXPECT_TRUE(check_reply(reply, call.xid, accept_stat::SYSTEM_ERR).empty());
  EXPECT_EQ(context.calls, 3U);
  call = call_msg<NullMeta>();
  reply = serve({});
  EXPECT_TRUE(check_reply(reply, call.xid, accept_stat::SYSTEM_ERR).empty());
  EXPECT_EQ(context.calls, 4U);
  EXPECT_EQ(logs.calls, 6U);
  EXPECT_EQ(logs.replies, 6U);
}

} // namespace

TEST(RpcDispatch, SynchronousReturnValuesAndErrors)
{
  SyncServer server;
  Context context;
  LogCounts logs;
  RpcDispatcher<blank_rpc_v1, SyncServer *, TestLogger, Context *> dispatcher(
      &server, TestLogger{&logs}, &context);
  check_dispatch(dispatcher, context, logs);
}

TEST(RpcDispatch, ReadyAwaitablesAndErrors)
{
  ReadyServer server;
  Context context;
  LogCounts logs;
  RpcDispatcher<blank_rpc_v1, ReadyServer *, TestLogger, Context *> dispatcher(
      &server, TestLogger{&logs}, &context);
  check_dispatch(dispatcher, context, logs);
}

TEST(RpcDispatch, SuspendedVoidAndValueResultsAndExceptions)
{
  for (bool returns_void : {false, true})
    for (bool fail : {false, true}) {
      SuspendedServer server;
      Context context{.fail = fail};
      LogCounts logs;
      RpcDispatcher<blank_rpc_v1, SuspendedServer *, TestLogger, Context *>
          dispatcher(&server, TestLogger{&logs}, &context);
      auto call = call_msg<EchoMeta>();
      call.xid = 456;
      auto args = serialize(Data{7});
      if (returns_void) {
        call.body.cbody().proc = 0;
        args.clear();
      }
      auto pending = dispatcher.serve(call, args);
      EXPECT_FALSE(pending.await_ready());
      EXPECT_EQ(context.calls, 0U);
      EXPECT_EQ(logs.calls, 1U);
      EXPECT_EQ(logs.replies, 0U);
      server.resume_();
      auto reply = pending();
      auto body =
          check_reply(reply, 456,
                      fail ? accept_stat::SYSTEM_ERR : accept_stat::SUCCESS);
      if (!returns_void && !fail) {
        Data out;
        deserialize(&out, body);
        EXPECT_EQ(out, (Data{7, 42}));
      }
      else
        EXPECT_TRUE(body.empty());
      EXPECT_EQ(context.calls, 1U);
      EXPECT_EQ(logs.replies, 1U);
    }
}

TEST(SrpcDispatch, ReturnValuesErrorsAndExtraArguments)
{
  SyncServer server;
  Context context;
  LogCounts logs;
  SRpcDispatcher<blank_rpc_v1, SyncServer *, TestLogger, Context *> dispatcher(
      &server, TestLogger{&logs}, &context);
  check_dispatch(dispatcher, context, logs);
}

TEST(SrpcDispatch, ErrorsWithoutLogger)
{
  SyncServer server;
  Context context{.fail = true};
  SRpcDispatcher<blank_rpc_v1, SyncServer *, RpcNoLogger, Context *> dispatcher(
      &server, {}, &context);
  auto call = call_msg<EchoMeta>();
  auto args = serialize(Data{1});
  auto reply = dispatcher.serve(call, args);
  EXPECT_TRUE(check_reply(reply, call.xid, accept_stat::SYSTEM_ERR).empty());
  reply = dispatcher.serve(call, {});
  EXPECT_TRUE(check_reply(reply, call.xid, accept_stat::GARBAGE_ARGS).empty());
  EXPECT_EQ(context.calls, 1U);
}

namespace {

template<typename T> struct FailingAllocator {
  using value_type = T;
  T *allocate(size_t) { throw std::bad_alloc{}; }
  void deallocate(T *p, size_t n) { std::allocator<T>{}.deallocate(p, n); }
};

struct AllocationVersion {
  static constexpr uint32_t program = blank_rpc_v1::program;
  static constexpr uint32_t version = 2;
  static constexpr const char *version_name = "allocation_v2";
  static constexpr std::array<uint32_t, 1> valid_procnos{0};

  template<uint32_t> struct proc_meta {
    using version_meta = AllocationVersion;
    static constexpr uint32_t proc = 0;
    static constexpr const char *proc_name = "allocate";
    using arg_type = Vec<uint8_t, 8, FailingAllocator<uint8_t>>;
    using return_type = void;
    using return_wire_type = std::array<uint8_t, 0>;

    static void dispatch(Context &context, arg_type &&) { context.invoke(); }
  };
};

} // namespace

TEST(RpcDispatch, AllocationFailureDuringDecodeDoesNotInvokeHandler)
{
  Context context;
  LogCounts logs;
  auto call = call_msg<AllocationVersion::proc_meta<0>>();
  auto args = serialize(uint32_t{1}, int32_t{7});
  RpcDispatcher<AllocationVersion, Context *, TestLogger> dispatcher(
      &context, TestLogger{&logs});
  auto reply = dispatcher.serve(call, args)();
  EXPECT_TRUE(check_reply(reply, call.xid, accept_stat::GARBAGE_ARGS).empty());
  EXPECT_EQ(context.calls, 0U);
  EXPECT_EQ(logs.calls, 1U);
  EXPECT_EQ(logs.replies, 1U);
}

TEST(SrpcDispatch, AllocationFailureDuringDecodeReturnsSystemError)
{
  Context context;
  LogCounts logs;
  auto call = call_msg<AllocationVersion::proc_meta<0>>();
  auto args = serialize(uint32_t{1}, int32_t{7});
  SRpcDispatcher<AllocationVersion, Context *, TestLogger> dispatcher(
      &context, TestLogger{&logs});
  auto reply = dispatcher.serve(call, args);
  EXPECT_TRUE(check_reply(reply, call.xid, accept_stat::SYSTEM_ERR).empty());
  EXPECT_EQ(context.calls, 0U);
  EXPECT_EQ(logs.calls, 1U);
  EXPECT_EQ(logs.replies, 1U);
}
