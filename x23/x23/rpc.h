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
//     auto client = ctream.client(myvers{});
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
//     rpc_stream_logged(cp, fd).server(&impl);
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
//     rpc_stream_logged(cp, fd).server(&impl, myvers{}, &context);
//
// To attach multiple servers:
//
//     rpc_stream_logged(cp, fd).server(&impl, myvers{}, &context)
//                              .server(&impl2, myothervers{}, &context);
// or
//     auto conf = rpc_stream_logged(cp, fd);
//     conf.server(&impl, myvers{}, &context);
//     conf.server(&impl2, myothervers{}, &context);
//     conf.go(); // Or let conf go out of scope to start serving
//
// Note rpc_stream and rpc_stream_logged will close the file
// descriptor when is shuts down.  A stream with no clients will close
// when the client closes the socket.  A stream with clients will
// close when the last client has been destroyed.

#pragma once

#include <x23/rpc_common.h>

#include <map>
#include <print>
#include <stop_token>

#include <x23/async.h>
#include <x23/corosync.h>
#include <x23/event.h>
#include <x23/fileio.h>
#include <x23/mvar.h>
#include <x23/serialize.h>
#include <x23/x23.h>

namespace x23 {

struct RpcService {
  virtual ~RpcService() {}
  virtual std::uint32_t program() const = 0;
  virtual std::uint32_t version() const = 0;
  virtual Async<Buf> serve(const rpc_msg &msg, BufView args SLOC_PARM) = 0;
  ProgVers getpv() const
  {
    return ProgVers{.prog = program(), .vers = version()};
  }
};

template<typename R> requires (!std::is_void_v<R>)
inline Async<R>
void_to_empty(Async<R> a)
{
  return a;
}
inline auto
void_to_empty(Async<void> a)
{
  struct VoidToEmpty {
    Async<void> a_;
    Awaitable<void> &a() { return a_.operator co_await(); }
    bool await_ready() noexcept { return a().await_ready(); }
    void await_suspend(std::coroutine_handle<> h SLOC_PARM) noexcept
    {
      a().await_suspend(h X23_IFTRACE(, sloc));
    }
    std::array<std::uint8_t, 0> await_resume()
    {
      a().await_resume();
      return {};
    }
  };
  return VoidToEmpty{std::move(a)};
}

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

  Async<Buf> serve(const rpc_msg &msg, BufView argdata SLOC_PARM) override
  {
    using enum msg_type;
    assert(msg.body.mtype() == CALL && msg.body.cbody().rpcvers == 2 &&
           msg.body.cbody().prog == program() &&
           msg.body.cbody().vers == version());
    return with_proc_meta<version_meta, Async<Buf>>(msg.body.cbody().proc,
                                                    Executor{*this, msg,
                                                             argdata});
  }

private:
  struct Executor {
    struct RpcDispatcher &d_;
    const rpc_msg &callmsg_;
    const BufView argbytes_;

    template<std::same_as<void>>
    Async<Buf> operator()(this Executor self SLOC_PARM)
    {
      using enum accept_stat;
      rpc_msg err{.xid = self.callmsg_.xid};
      msg_set_reply_stat(&err, PROC_UNAVAIL);
      self.d_.logger_.template log_server<VersMeta>(self.callmsg_);
      self.d_.logger_.template log_server<VersMeta>(err);
      co_return make_record(err);
    }
    template<typename ProcMeta>
    Async<Buf> operator()(this Executor self SLOC_PARM)
    {
      using enum accept_stat;
      typename ProcMeta::arg_type args;
      rpc_msg rmsg{.xid = self.callmsg_.xid};
      try {
        deserialize(&args, self.argbytes_);
      } catch (const XdrException &e) {
        msg_set_reply_stat(&rmsg, GARBAGE_ARGS);
        if constexpr (!std::same_as<Logger, RpcNoLogger>) {
          self.d_.logger_.template log_server<ProcMeta>(self.callmsg_);
          self.d_.logger_.template log_server<ProcMeta>(
              rmsg, uformat(" deserialization error: {}", e.what()));
        }
        co_return make_record(rmsg);
      }
      self.d_.logger_.template log_server<ProcMeta>(self.callmsg_, args);
      try {
        auto res = co_await void_to_empty(std::apply(
            [&](auto &...ea) {
              return ProcMeta::dispatch(*self.d_.impl_, args, ea...);
            },
            self.d_.ea_));
        msg_set_reply_stat(&rmsg, SUCCESS);
        self.d_.logger_.template log_server<ProcMeta>(rmsg, res);
        co_return make_record(rmsg, res);
      } catch (const std::exception &e) {
        msg_set_reply_stat(&rmsg, SYSTEM_ERR);
        std::println(stderr, "{}::{}: {}", ProcMeta::version_meta::version_name,
                     ProcMeta::proc_name, e.what());
        if constexpr (!std::same_as<Logger, RpcNoLogger>)
          self.d_.logger_.template log_server<ProcMeta>(rmsg,
                                                        uformat(" {}",
                                                                e.what()));
        co_return make_record(rmsg);
      }
    }
  };
};

template<typename T, typename VersMeta, typename... ExtraArgs>
concept implements_async_rpc = implements_rpc<T, VersMeta, Async, ExtraArgs...>;

namespace detail {
class RpcStreamImpl : public std::enable_shared_from_this<RpcStreamImpl> {
public:
  size_t max_record_size = 1UZ << 24;
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
  Detached write_record(Buf b SLOC_PARM);
  Detached reader(SLOC_PARM0);

private:
  struct ClientState {
    std::uint32_t lastxid_{};
    std::map<std::uint32_t, CoroCB<Buf>> xidmap_;
  };
  MVar<ClientState> cstate_;

  void send_msg(const rpc_msg &msg, const is_xdr_type auto &...x)
  {
    write_record(make_record(msg, x...));
  }

  Async<std::expected<std::uint32_t, Errno>> read_uint(SLOC_PARM0);
  Async<std::expected<Buf, Errno>> read_record(SLOC_PARM0);

  Detached serve_call(const rpc_msg &call, BufView args SLOC_PARM);
};
} // namespace detail

template<typename Logger = RpcNoLogger>
struct RpcStream : public detail::RpcStreamImpl {
  using super = detail::RpcStreamImpl;

  Logger logger_;

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
    write_record(std::move(record));

    Buf rbuf = co_await a;
    BufView rv = rbuf.subspan(deserialize_from(&msg, rbuf));
    if (!rpc_is_success(msg)) {
      logger_.log_client(msg);
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

    logger_.log_client(msg, ret);
    if constexpr (std::is_void_v<typename ProcMeta::return_type>)
      co_return;
    else
      co_return ret;
  }

  std::shared_ptr<RpcStream> get_ref()
  {
    return static_pointer_cast<RpcStream>(shared_from_this());
  }

  struct ClientRef : std::shared_ptr<RpcStream> {
    using super = std::shared_ptr<RpcStream>;
    ClientRef(RpcStream &s) : super(s.get_ref())
    {
      this->get()->nclients_.fetch_add(1, std::memory_order_relaxed);
    }
    ClientRef(const ClientRef &s) : super(s)
    {
      this->get()->nclients_.fetch_add(1, std::memory_order_relaxed);
    }
    ~ClientRef()
    {
      if (this->get()->nclients_.fetch_sub(1, std::memory_order_relaxed) == 1)
        this->get()->fail();
    }
  };

  /*
  template<typename VersMeta> requires requires { typename VersMeta::client; }
  typename VersMeta::template client<ClientRef> client(VersMeta = {})
  {
    return typename VersMeta::client{ClientRef{*this}};
  }
  */

  struct Configurator {
    Configurator(std::shared_ptr<RpcStream> s) noexcept : s_(std::move(s)) {}
    Configurator(const Configurator &) = delete;
    ~Configurator() { go(); }

    template<typename Server,
             typename VersMeta = typename std::remove_cvref_t<
                 decltype(*std::declval<Server>())>::rpc_service,
             typename... ExtraArgs>
    requires implements_async_rpc<Server, VersMeta, ExtraArgs...>
    Configurator &server(Server srvr, VersMeta = {}, ExtraArgs &&...extra_args)
    {
      assert(!reader_running_);
      auto dispatch = new RpcDispatcher<VersMeta, Server, Logger,
                                        std::decay_t<ExtraArgs>...>(
          std::move(srvr), s_->logger_, std::forward<ExtraArgs>(extra_args)...);
      s_->srvmap_.emplace(dispatch->getpv(), dispatch);
      return *this;
    }
    template<typename VersMeta, typename Server, typename... ExtraArgs>
    requires implements_async_rpc<Server, VersMeta, ExtraArgs...>
    Configurator &server(Server srvr, ExtraArgs &&...ea)
    {
      return server<Server, VersMeta, ExtraArgs...>(std::move(srvr), {},
                                                    std::forward<ExtraArgs>(
                                                        ea)...);
    }

    template<typename VersMeta>
    requires requires { typename VersMeta::template client<ClientRef>; }
    typename VersMeta::template client<ClientRef> client(VersMeta = {})
    {
      go();
      return typename VersMeta::template client<ClientRef>{ClientRef{*s_}};
    }
    void go()
    {
      if (!reader_running_) {
        reader_running_ = true;
        s_->reader();
      }
    }

  private:
    bool reader_running_{false};
    std::shared_ptr<RpcStream> s_;
  };

  static Configurator make(CoroPoll &cp, UniqueFd fd, Logger logger = {})
  {
    return std::make_shared<RpcStream>(cp, std::move(fd), std::move(logger),
                                       ConstructorKey{});
  }

protected:
  struct ConstructorKey {};

public:
  using super::super;
  RpcStream(CoroPoll &cp, UniqueFd fd, Logger logger, ConstructorKey)
    : super(cp, std::move(fd)), logger_(std::move(logger))
  {}
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
inline RpcStream<RpcLogger>::Configurator
rpc_stream_logged(CoroPoll &cp, int fd, int logfd = RpcLogger::default_logfd)
{
  return RpcStream<RpcLogger>::make(cp, UniqueFd{fd}, RpcLogger(fd, logfd));
}

} // namespace x23
