#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <stdexcept>
#include <sys/socket.h>

#include "compat_probe.hh"
#include "x23/rpc.h"
#include "x23/threadpool.h"

namespace x23 {
namespace {

ThreadPool threadpool(2);
CoroPoll cp(threadpool);

struct CompatProbe1Impl {
  using rpc_service = CompatProbe1;

  Async<void> ping() { co_return; }
};

struct CompatProbe2Impl {
  using rpc_service = CompatProbe2;

  Async<void> ping() { co_return; }
  Async<void> added(std::int32_t) { co_return; }
};

template<std::uint32_t Version, std::uint32_t Procedure> struct VoidCall {
  struct version_meta {
    static constexpr std::uint32_t program = CompatProbe1::program;
    static constexpr std::uint32_t version = Version;
    static constexpr const char *version_name = "CompatProbeRaw";
  };

  static constexpr std::uint32_t proc = Procedure;
  static constexpr const char *proc_name = "raw";
  using return_type = void;
  using return_wire_type = std::array<std::uint8_t, 0>;
};

rpc_msg
expect_failure(auto &&call, accept_stat expected)
{
  try {
    call();
    ADD_FAILURE() << "RPC call unexpectedly succeeded";
  } catch (const XdrCallFailed &e) {
    EXPECT_EQ(e.m_.body.mtype(), msg_type::REPLY);
    EXPECT_EQ(e.m_.body.rbody().stat(), reply_stat::MSG_ACCEPTED);
    EXPECT_EQ(e.m_.body.rbody().areply().reply_data.stat(), expected);
    return e.m_;
  }
  throw std::runtime_error("RPC call did not produce XdrCallFailed");
}

TEST(CompatProbe, NativeProgramVersionProcedureNegotiation)
{
  int fds[2];
  ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);

  CompatProbe1Impl v1_impl;
  CompatProbe2Impl v2_impl;
  auto server = rpc_stream(cp, fds[1]);
  server->server(&v1_impl, CompatProbe1{});
  server->server(&v2_impl, CompatProbe2{});
  server->go();

  auto client_stream = rpc_stream(cp, fds[0]);
  auto v1 = client_stream->client(CompatProbe1{});
  auto v2 = client_stream->client(CompatProbe2{});

  EXPECT_NO_THROW(v1.ping()());
  EXPECT_NO_THROW(v2.ping()());
  EXPECT_NO_THROW(v2.added(42)());

  using BelowRange = VoidCall<CompatProbe1::version - 1, 0>;
  rpc_msg below =
      expect_failure([&] { client_stream->send_call<BelowRange>()(); },
                     accept_stat::PROG_MISMATCH);
  EXPECT_EQ(below.body.rbody().areply().reply_data.mismatch_info().low,
            CompatProbe1::version);
  EXPECT_EQ(below.body.rbody().areply().reply_data.mismatch_info().high,
            CompatProbe2::version);

  using AboveRange = VoidCall<CompatProbe2::version + 1, 0>;
  rpc_msg above =
      expect_failure([&] { client_stream->send_call<AboveRange>()(); },
                     accept_stat::PROG_MISMATCH);
  EXPECT_EQ(above.body.rbody().areply().reply_data.mismatch_info().low,
            CompatProbe1::version);
  EXPECT_EQ(above.body.rbody().areply().reply_data.mismatch_info().high,
            CompatProbe2::version);

  using AddedOnV1 = VoidCall<CompatProbe1::version, 1>;
  expect_failure([&] { client_stream->send_call<AddedOnV1>()(); },
                 accept_stat::PROC_UNAVAIL);

  using MissingAddedArgument = VoidCall<CompatProbe2::version, 1>;
  expect_failure([&] { client_stream->send_call<MissingAddedArgument>()(); },
                 accept_stat::GARBAGE_ARGS);
}

} // namespace
} // namespace x23
