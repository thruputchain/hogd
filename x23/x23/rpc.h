// -*-C++-*-

// This module implements the asynchronous coroutine-based RPC interface.
//
// To make remote procedure calls on a stream socket, you must create
// an RpcStream with one of the two functions rpc_stream or
// rpc_stream_logged.  These functions return a "Configurator" object
// that you can use to register servers or create clients on the
// socket.  You must register servers first before clients, and you
// must destroy the configurator or call go() on it to start listening
// for incoming calls.  (This is to avoid rejecting calls for
// program/versions you are about to register.)
//
// In a .x file, consider a declaration like this:
//
//     program myprog {
//       version myvers {
//         void null(void) = 0;
//         int add1(int) = 1;
//       } = 1;
//     } = 3123123;
//
// To act as a client over a TCP socket fd, you can use the following:
//
// // globally:
// ThreadPool pool;
// CoroPoll cp(pool);
//
//     ...
//     auto cstream = rpc_stream_logged(cp, fd);
//     auto client = ctream->client(myvers{});
//     client.null()(); // First function returns async, second waits for it
//     int two = client.add1(1)();
//
// On the server side, you can run:
//
// ThreadPool pool;
// CoroPoll cp(pool);
//
// struct Impl {
//   using rpc_service = myvers;
//   Async<void> null() { co_return; }
//   Async<std::int32_t> add1(std::int32_t i) { co_return i + 1; }
// };
// Impl impl;    // global implementation of service
//
//     ...
//     rpc_stream_logged(cp, fd)->server(&impl);
//
// You can supply an extra context argument to every server method if
// you supply it as a third argument after the RPC version object:
//
// struct Context { /* ... */ };
// Context context;
//
// struct Impl {
//   using rpc_service = myvers;
//   Async<void> null(Context *c) { co_return; }
//   Async<std::int32_t> add1(std::int32_t i, Context *c) { co_return i + 1; }
// };
// Impl impl;    // global implementation of service
//
//     ...
//     rpc_stream_logged(cp, fd)->server(&impl, myvers{}, &context);
//
// To attach multiple servers:
//
//     rpc_stream_logged(cp, fd)->server(&impl, myvers{}, &context)
//                              ->server(&impl2, myothervers{}, &context);
// or
//     auto conf = rpc_stream_logged(cp, fd);
//     conf->server(&impl, myvers{}, &context);
//     conf->server(&impl2, myothervers{}, &context);
//     conf->go(); // Or let conf go out of scope to start serving
//
// Note rpc_stream and rpc_stream_logged will close the file
// descriptor when is shuts down.  A stream with no clients will close
// when the client closes the socket.  A stream with clients will
// close when the last client has been destroyed.

#pragma once

#include <atomic>
#include <cassert>
#include <concepts>
#include <cstdint>
#include <exception>
#include <expected>
#include <map>
#include <print>
#include <stop_token>
#include <tuple>
#include <type_traits>
#include <utility>

#include <x23/async.h>
#include <x23/cleanup.h>
#include <x23/corosync.h>
#include <x23/corotrace.h>
#include <x23/err.h>
#include <x23/event.h>
#include <x23/fileio.h>
#include <x23/mvar.h>
#include <x23/refcnt.h>
#include <x23/rpc_common.h> // IWYU pragma: export
#include <x23/rpc_msg.hh>
#include <x23/serialize.h>
#include <x23/sockio.h>
#include <x23/utility.h>
#include <x23/vec.h>
#include <x23/x23.h>

namespace x23 {

struct RpcService {
  virtual ~RpcService() {}
  virtual std::uint32_t program() const = 0;
  virtual std::uint32_t version() const = 0;
  virtual MaybeAsync<Buf> serve(const rpc_msg &msg, BufViewEnd args) = 0;
  ProgVers getpv() const
  {
    return ProgVers{.prog = program(), .vers = version()};
  }
};

template<typename VersMeta, typename Impl, typename Logger = RpcNoLogger,
         typename... ExtraArgs>
struct RpcDispatcher : RpcService {
  using version_meta = VersMeta;
  Impl impl_;
  [[no_unique_address]] Logger logger_;
  [[no_unique_address]] std::tuple<ExtraArgs...> ea_;

  RpcDispatcher(Impl impl, Logger log, ExtraArgs... ea)
    : impl_(std::move(impl)), logger_(log), ea_(std::move(ea)...)
  {}

  std::uint32_t program() const final { return version_meta::program; }
  std::uint32_t version() const final { return version_meta::version; }

  MaybeAsync<Buf> serve(const rpc_msg &msg, BufViewEnd argdata) override
  {
    using enum msg_type;
    assert(msg.body.mtype() == CALL && msg.body.cbody().rpcvers == 2 &&
           msg.body.cbody().prog == program() &&
           msg.body.cbody().vers == version());
    return with_proc_meta<version_meta, MaybeAsync<Buf>>(
        msg.body.cbody().proc, [&]<typename ProcMeta>() -> MaybeAsync<Buf> {
          if constexpr (std::is_void_v<ProcMeta>)
            return proc_unavail(msg);
          else
            return ProcHandler<ProcMeta>{*this, msg}(argdata);
        });
  }

private:
  Buf proc_unavail(const rpc_msg &callmsg)
  {
    using enum accept_stat;
    rpc_msg err{.xid = callmsg.xid};
    msg_set_reply_stat(&err, PROC_UNAVAIL);
    logger_.template log_server<VersMeta>(callmsg);
    logger_.template log_server<VersMeta>(err);
    return make_record(err);
  }

  template<typename ProcMeta> struct ProcHandler {
    using arg_type = ProcMeta::arg_type;
    using return_type = ProcMeta::return_type;

    RpcDispatcher &d_;
    std::uint32_t xid_;
    const rpc_msg *callmsg_{};

    ProcHandler(RpcDispatcher &d, const rpc_msg &callmsg)
      : d_(d), xid_(callmsg.xid), callmsg_(&callmsg)
    {}

    using enum accept_stat;

    MaybeAsync<Buf> operator()(BufViewEnd argbytes)
    try {
      constexpr bool returns_void = std::is_void_v<return_type>;
      using dispatch_t = decltype(dispatch(argbytes));
      rpc_msg rmsg{.xid = xid_};
      msg_set_reply_stat(&rmsg, SUCCESS);

      if constexpr (!is_awaitable<dispatch_t>) {
        if constexpr (returns_void) {
          dispatch(argbytes);
          d_.logger_.template log_server<ProcMeta>(rmsg);
          return make_record(rmsg);
        }
        else {
          return_type res = dispatch(argbytes);
          d_.logger_.template log_server<ProcMeta>(rmsg, res);
          return make_record(rmsg, res);
        }
      }
      else {
        auto awaitable = dispatch(argbytes);
        decltype(auto) awaiter = detail::get_awaiter(awaitable);
        if (awaiter.await_ready()) {
          if constexpr (returns_void) {
            awaiter.await_resume();
            d_.logger_.template log_server<ProcMeta>(rmsg);
            return make_record(rmsg);
          }
          else {
            return_type res = awaiter.await_resume();
            d_.logger_.template log_server<ProcMeta>(rmsg, res);
            return make_record(rmsg, res);
          }
        }

        // Dispatch returned something awaitable and it's not ready,
        // so here we actually need to allocate a new coroutine frame.
        return [](ProcHandler self /* copy *this into coroutine */,
                  auto awaitable) noexcept -> Async<Buf> {
          try {
            rpc_msg rmsg{.xid = self.xid_};
            msg_set_reply_stat(&rmsg, SUCCESS);
            if constexpr (returns_void) {
              co_await awaitable;
              self.d_.logger_.template log_server<ProcMeta>(rmsg);
              co_return make_record(rmsg);
            }
            else {
              return_type res = co_await awaitable;
              self.d_.logger_.template log_server<ProcMeta>(rmsg, res);
              co_return make_record<return_type>(rmsg, res);
            }
          } catch (const std::exception &e) {
            co_return self.error(e);
          }
        }(*this, std::move(awaitable));
      }
    } catch (std::exception &e) {
      return error(e);
    }

    decltype(auto) dispatch(BufViewEnd argbytes)
    {
      return std::apply(
          [&](auto &...ea) {
            return ProcMeta::dispatch(*d_.impl_, args(argbytes), ea...);
          },
          d_.ea_);
    }

    arg_type args(BufViewEnd argbytes)
    {
      arg_type args;
      deserialize(&args, argbytes);
      d_.logger_.template log_server<ProcMeta>(*callmsg_, args);
      // callmsg_ non-null is used to detect that an exception was
      // thrown during deserialization of arguments.  Also, if we
      // suspend a coroutine, callmsg_ will no longer be valid anyway.
      callmsg_ = nullptr;
      return args;
    }

    Buf result(const typename ProcMeta::return_wire_type &res) const noexcept
    {
      rpc_msg rmsg{.xid = xid_};
      msg_set_reply_stat(&rmsg, SUCCESS);
      d_.logger_.template log_server<ProcMeta>(rmsg, res);
      try {
        return make_record(rmsg, res);
      } catch (const std::exception &e) {
        return error(e);
      }
    }
    Buf result() const noexcept
    {
      return reply(typename ProcMeta::return_wire_type{});
    }
    Buf error(const std::exception &e) const
    {
      rpc_msg rmsg{.xid = xid_};
      if (callmsg_) {
        msg_set_reply_stat(&rmsg, GARBAGE_ARGS);
        if constexpr (!std::same_as<Logger, RpcNoLogger>) {
          d_.logger_.template log_server<ProcMeta>(*callmsg_);
          d_.logger_.template log_server<ProcMeta>(
              rmsg, uformat(" deserialization error: {}", e.what()));
        }
      }
      else {
        msg_set_reply_stat(&rmsg, SYSTEM_ERR);
        std::println(stderr, "{}::{}: {}", ProcMeta::version_meta::version_name,
                     ProcMeta::proc_name, e.what());
        if constexpr (!std::same_as<Logger, RpcNoLogger>)
          d_.logger_.template log_server<ProcMeta>(rmsg,
                                                   uformat(" {}", e.what()));
      }
      return make_record(rmsg);
    }
  };
};

template<typename T, typename VersMeta, typename... ExtraArgs>
concept implements_async_rpc =
    implements_rpc<T, VersMeta, MaybeAsync, ExtraArgs...>;

namespace detail {
class RpcStreamImpl : public RefCount {
public:
  size_t max_message_size = default_max_message_size;
  RefPtr<CoroPoll> cp_;

  void fail();

protected:
  UniqueFd fd_;
  std::stop_source ss_;
  std::atomic<unsigned> nclients_{0};
  SrvMap<RpcService> srvmap_;

  RpcStreamImpl(CoroPoll &cp, UniqueFd fd);
  virtual ~RpcStreamImpl();
  [[nodiscard]] std::uint32_t gen_call_xid(Awaitable<Buf> &);
  Detached reader(SLOC_PARM0);
  virtual void log_message(std::string msg) = 0;

private:
  struct ClientState {
    std::uint32_t lastxid_{};
    std::map<std::uint32_t, CoroCB<Buf>> xidmap_;
  };
  MVar<ClientState> cstate_;

  void send_msg(const rpc_msg &msg, const is_xdr_type auto &...x)
  {
    cp_->awrite(fd_, make_record(msg, x...));
  }

  Async<std::expected<std::uint32_t, Errno>> read_uint(SLOC_PARM0);
  Async<Errno> read_record(Buf *b SLOC_PARM);

  friend class RefCount;
};
} // namespace detail

template<typename Logger = RpcNoLogger>
struct RpcStream : public detail::RpcStreamImpl {
  using super = detail::RpcStreamImpl;
  static constexpr bool kAllowRefPtrConversion = true;

  Logger logger_;
  bool reader_running_{false};

  RpcStream(CoroPoll &cp, UniqueFd fd, Logger logger)
    : super(cp, std::move(fd)), logger_(std::move(logger))
  {}

  struct ClientRef : RefPtr<RpcStream> {
    using super = RefPtr<RpcStream>;
    using super::get;
    ClientRef(RpcStream &s) : super(s)
    {
      get()->nclients_.fetch_add(1, std::memory_order_relaxed);
    }
    ClientRef(const ClientRef &s) : super(s)
    {
      get()->nclients_.fetch_add(1, std::memory_order_relaxed);
    }
    ~ClientRef()
    {
      if (get()->nclients_.fetch_sub(1, std::memory_order_relaxed) == 1)
        get()->fail();
    }
  };

  template<typename VersMeta>
  requires requires { typename VersMeta::template client<ClientRef>; }
  typename VersMeta::template client<ClientRef> client(VersMeta = {})
  {
    go();
    return typename VersMeta::template client<ClientRef>{ClientRef{*this}};
  }

  template<typename Server,
           typename VersMeta = typename std::remove_cvref_t<
               decltype(*declval<Server>())>::rpc_service,
           typename... ExtraArgs>
  requires implements_async_rpc<Server, VersMeta, ExtraArgs...>
  RpcStream *server(Server srvr, VersMeta = {}, ExtraArgs &&...extra_args)
  {
    auto dispatch =
        new RpcDispatcher<VersMeta, Server, Logger, std::decay_t<ExtraArgs>...>(
            std::move(srvr), logger_, std::forward<ExtraArgs>(extra_args)...);
    srvmap_.emplace(dispatch->getpv(), dispatch);
    logger_.log_message("serving {}", VersMeta::version_name);
    return this;
  }
  template<typename VersMeta, typename Server, typename... ExtraArgs>
  requires implements_async_rpc<Server, VersMeta, ExtraArgs...>
  RpcStream *server(Server srvr, ExtraArgs &&...ea)
  {
    return server<Server, VersMeta, ExtraArgs...>(std::move(srvr), {},
                                                  std::forward<ExtraArgs>(
                                                      ea)...);
  }

  void go()
  {
    if (!reader_running_) {
      reader_running_ = true;
      logger_.log_message("launching reader");
      reader();
      logger_.log_message("reader launched");
    }
  }

  struct Configurator : public RefPtr<RpcStream> {
    using super = RefPtr<RpcStream>;
    using super::get;

    Configurator(RpcStream &s) noexcept : super(s) {}
    Configurator(Configurator &&) noexcept = default;
    Configurator(const Configurator &) = delete;
    ~Configurator()
    {
      if (RpcStream *s = get())
        s->go();
    }
  };

  template<typename ProcMeta>
  Async<typename ProcMeta::return_type>
  send_call(const is_xdr_type auto &...args)
  {
    auto msg = call_msg<ProcMeta>();
    Buf record = make_record(msg, args...);

    // Note we geneate the xid after calling make_record just in case
    // make_record throws because of illegal XDR values (such as an
    // invalid union discriminant).
    Awaitable<Buf> a;
    putbe(record.data() + 4, msg.xid = gen_call_xid(a));

    logger_.template log_client<ProcMeta>(msg, std::forward_as_tuple(args...));
    cp_->awrite(fd_, std::move(record));

    Buf rbuf = co_await a;
    BufView rv = rbuf.subspan(deserialize_from(&msg, rbuf));
    if (!rpc_is_success(msg)) {
      logger_.template log_client<ProcMeta>(msg);
      throw XdrCallFailed(msg);
    }

    typename ProcMeta::return_wire_type ret;
    try {
      deserialize(&ret, rv);
    } catch (XdrException &e) {
      if constexpr (!std::same_as<Logger, RpcNoLogger>)
        logger_.template log_client<ProcMeta>(
            msg, uformat(" deserialization error: {}", e.what()));
      throw e;
    }

    logger_.template log_client<ProcMeta>(msg, ret);
    if constexpr (std::is_void_v<typename ProcMeta::return_type>)
      co_return;
    else
      co_return ret;
  }

  static Configurator make(CoroPoll &cp, UniqueFd fd, Logger logger = {})
  {
    return *new RpcStream(cp, std::move(fd), std::move(logger));
  }

  void log_message(std::string msg) override { logger_.log_message("{}", msg); }
};

// The return type of RpcStream<Logger>::client<VersMeta>, for
// situations where you cannot use auto.
template<typename VersMeta, typename Logger = RpcNoLogger,
         typename... ExtraArgs>
using RpcClient =
    VersMeta::template client<typename RpcStream<Logger>::ClientRef,
                              ExtraArgs...>;

// Create an RPC Stream attached to a file descriptor.  Returns a
// Configurator object with which you can add servers and create
// clients, e.g.,
//
//     auto conf = rpc_stream(cp, fd);
//     conf.server(&impl, myvers{});
//     myvers::client c = conf.client(myvers{});
//
// The RpcStream will close file descriptor fd.
template<typename Logger = RpcNoLogger>
typename RpcStream<Logger>::Configurator
rpc_stream(CoroPoll &cp, int fd, Logger logger = {})
{
  return RpcStream<Logger>::make(cp, UniqueFd{fd}, std::move(logger));
}

// Like rpc_stream, but creates a logging RpcStream.  The third
// argument, logfd, is where to write a plain text transcript of calls
// and replies.  When selecting whether to log at runtime, you can
// pass logfd = -1 to suppress logging and most of the logging
// overhead.
template<typename Logger = RpcLogger>
inline RpcStream<Logger>::Configurator
rpc_stream_logged(CoroPoll &cp, int fd, int logfd = RpcLogger::default_logfd)
{
  return RpcStream<Logger>::make(cp, UniqueFd{fd}, Logger(fd, logfd));
}

// Listen on a TCP port, and for each incoming connection invoke a
// callback with a new configurator.  Sets logging based on the
// X23SERVER_TRACE_progname or X23SERVER_TRACE environment variable.
// See RpcTraceMode::getenv for a description of this environment
// variable.
//
// Returns a pair consisting of the port number on which it is
// listening and the listening file descriptor.  The only reasonable
// thing to do for the listening file descriptor is to poison it when
// you want to shut the server down, i.e.:
//
//   auto [port, fd] = tcp_rpc_server(cp, 0, [](auto c) {
//        c->server(&impl, myvers{});
//     });
//
//   // ...
//   // later on to shut down server
//   cp.poison(fd);
std::pair<uint16_t, BorrowedFd>
tcp_rpc_server(CoroPoll &cp, uint16_t port, auto cb, int backlog = -1)
    requires std::invocable<decltype(cb), RpcStream<RpcNoLogger>::Configurator>
{
  if (backlog < 0)
    backlog = 5 + cp.pool_.nthreads_;
  auto lfd = tcplisten(port, backlog);
  int rawfd = lfd.get();
  if (port == 0)
    port = get_port(local_addr(lfd));

  [&cp, lfd = std::move(lfd),
   cb = std::move(cb)](this auto /*copy*/) -> Detached {
    Defer _resetfd{[&lfd, &cp] { cp.reset(lfd); }};
    while (auto c = co_await (cp.accept(lfd))) {
      server_trace().with_logger(*c, [&](auto logger) {
        std::invoke(cb, RpcStream<decltype(logger)>::make(cp, UniqueFd{*c},
                                                          std::move(logger)));
      });
    }
  }();

  return std::pair{port, rawfd};
}

// Similar to tcp_rpc_server, but doesn't return unless/until it
// accept returns an error or the CoroPoll is destroyed.
void
tcp_rpc_server_wait(CoroPoll &cp, uint16_t port, auto cb, int backlog = -1)
    requires std::invocable<decltype(cb), RpcStream<RpcNoLogger>::Configurator>
{
  std::mutex m;
  tcp_rpc_server(
      cp, port,
      [cb = std::move(cb), _ul = std::unique_lock{m}](auto configurator) {
        std::invoke(cb, std::move(configurator));
      },
      backlog);
  std::unique_lock _ul(m);
}

} // namespace x23
