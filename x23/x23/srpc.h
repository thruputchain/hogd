// -*-C++-*-

// This module implements simple synchronous RPCs.  Each file
// descriptor has two threads, a read thread and a write thread.
//
// On the server side, all RPC methods run to completion in the read
// thread.  The server reads requests in batches, and wakes the write
// thread once it has finished processing a batch.
//
// On the client side, each call immediately wakes the write thread.
// Results are unmarshalled in the read thread before being returned
// to the caller.
//
// A simple example client:
//
//   UniqueFd fd = x23::tcpconnect(HostPort{"localhost", 1234}.to_addr(true));
//   auto c = x23::make_rpc_stream(fd.release())->client(blank_rpc_v1{});
//   c->null();  ;; makes remote procedure call
//
// A simple server:
//
//   struct Server {
//     using rpc_service = blank_rpc_v1;
//
//     void null() {}
//     Data echo(Data arg) { return arg; }
//   };
//   Server server;
//
//   int fd = accept(listen_fd, nullptr, nullptr);
//   if (fd == -1)
//     x23::Errno.raise("accept");
//   srpc_stream_logged(fd)->server(&server);
//
// You can have both clients and servers on the same SRpcStream.  By
// default, if there are any clients, then the stream will be
// destroyed when the last client reference is destroyed.  If there
// are no clients, then the server will be destroyed when it reads an
// EOF or error from the file descriptor.  On a server, you can also
// get a reference with get_fd(), and then the SRpcStream will be
// deleted when you drop the reference.

#pragma once

#include <print>
#include <semaphore>
#include <stop_token>
#include <thread>

#include <x23/cleanup.h>
#include <x23/cpumap.h>
#include <x23/mvar.h>
#include <x23/refcnt.h>
#include <x23/rpc_common.h> // IWYU pragma: export
#include <x23/sockio.h>
#include <x23/uiobuf.h>
#include <x23/vec.h>

namespace x23 {

struct SRpcService {
  virtual ~SRpcService() {}
  virtual std::uint32_t program() const = 0;
  virtual std::uint32_t version() const = 0;
  virtual Buf serve(const rpc_msg &msg, BufView args) = 0;
  ProgVers getpv() const
  {
    return ProgVers{.prog = program(), .vers = version()};
  }
};

class SRpcFd : public RefCount {
public:
  static inline bool ignore_sigpipe = true;

  // Note the BufView argument to CallCB is only the arguments, not
  // the rpc_msg, which has already been decoded.  (This is an
  // asymmetry with write_call, in which the Buf argument contains the
  // record length, rpc_msg, and arguments.)
  using CallCB = std::function<void(rpc_msg *, BufView)>;

  // Note if you change this, it don't set it larger than 0x7fffffff
  // because we don't support chunked messages.
  static inline std::uint32_t default_max_message_size = 0x100'0000;
  size_t max_message_size_;
  SrvMap<SRpcService> srvmap_{};

  SRpcFd(UniqueFd fd);
  SRpcFd(const SRpcFd &) = delete;

  template<typename... Args>
  void fail(std::format_string<Args...> fmt, Args &&...args)
  {
    Str msg = uformat("SRpcFd(fd {}): ", fd_.get()) +
              uvformat(fmt.get(), std::make_format_args(args...)) + "\n";
    ::write(2, msg.data(), msg.size());
    stop();
  }

  void go(const CpuSet *cpus = nullptr);
  void stop() noexcept;

protected:
  using Xid = std::uint32_t;

  virtual ~SRpcFd();

  // Enqueue a serialized call. record must be a complete RPC record
  // including length, rpc_msg, and arguments.  However, if the XID
  // inside the rpc_msg is 0 (or not unique) it will be overwritten
  // with a unique value at the time the RPC is sent.
  void write_call(Buf record, CallCB cb);

  // Get a probably unique Xid for debug logging purposes.  It's more
  // efficient to leave the Xid as 0 and let the write_worker fill it
  // in, thereby avoiding any contention on calls_.
  Xid genxid();

private:
  static constexpr size_t kReadSize = 0x10000;

  struct PendingCalls {
    Xid lastxid_;
    std::unordered_map<Xid, CallCB> cbmap_;
  };
  MVar<PendingCalls> calls_;

  struct WriteItem {
    Buf data_;
    CallCB replycb_;
  };
  MVar<Vec<WriteItem>> write_queue_;
  std::atomic_bool write_ready_{false};

  UniqueFd fd_;
  std::atomic_int refcnt_{0};
  std::stop_source ss_;
  std::thread read_thread_;
  std::thread write_thread_;

  void read_worker(const CpuSet *) noexcept;
  void input(BufView) noexcept;
  void write_worker(const CpuSet *, RefPtr<SRpcFd>) noexcept;
  void write_ready() noexcept
  {
    if (!write_ready_.exchange(true))
      write_ready_.notify_one();
  }

  friend class RefCount;
};

namespace detail {
inline auto
void_to_0tuple(auto &&f)
{
  if constexpr (std::is_void_v<decltype(f())>) {
    f();
    return std::tuple<>{};
  }
  else
    return f();
}
} // namespace detail

template<typename VersMeta, typename Impl, typename Logger = RpcNoLogger,
         typename... ExtraArgs>
struct SRpcDispatcher : SRpcService {
  using version_meta = VersMeta;
  Impl impl_;
  [[no_unique_address]] Logger logger_;
  [[no_unique_address]] std::tuple<ExtraArgs...> ea_;

  SRpcDispatcher(Impl impl, Logger log, ExtraArgs... ea)
    : impl_(std::move(impl)), logger_(log), ea_(std::move(ea)...)
  {}

  std::uint32_t program() const final { return version_meta::program; }
  std::uint32_t version() const final { return version_meta::version; }

  Buf serve(const rpc_msg &msg, BufView argdata) override
  {
    using enum msg_type;
    assert(msg.body.mtype() == CALL && msg.body.cbody().rpcvers == 2 &&
           msg.body.cbody().prog == program() &&
           msg.body.cbody().vers == version());
    return with_proc_meta<version_meta, Buf>(msg.body.cbody().proc,
                                             Executor{*this, msg, argdata});
  }

private:
  struct Executor {
    struct SRpcDispatcher &d_;
    const rpc_msg &callmsg_;
    const BufView argbytes_;

    template<std::same_as<void>> Buf operator()() const
    {
      using enum accept_stat;
      rpc_msg err{.xid = callmsg_.xid};
      msg_set_reply_stat(&err, PROC_UNAVAIL);
      d_.logger_.template log_server<VersMeta>(callmsg_);
      d_.logger_.template log_server<VersMeta>(err);
      return make_record(err);
    }
    template<typename ProcMeta> Buf operator()() const
    {
      using enum accept_stat;
      typename ProcMeta::arg_type args;
      rpc_msg rmsg{.xid = callmsg_.xid};
      try {
        deserialize(&args, argbytes_);
      } catch (const XdrException &e) {
        msg_set_reply_stat(&rmsg, GARBAGE_ARGS);
        if constexpr (!std::same_as<Logger, RpcNoLogger>) {
          d_.logger_.template log_server<ProcMeta>(callmsg_);
          d_.logger_.template log_server<ProcMeta>(
              rmsg, uformat(" deserialization error: {}", e.what()));
        }
        return make_record(rmsg);
      }
      d_.logger_.template log_server<ProcMeta>(callmsg_, args);
      try {
        auto res = detail::void_to_0tuple([&] {
          return std::apply(
              [&](auto &...ea) {
                return ProcMeta::dispatch(*d_.impl_, args, ea...);
              },
              d_.ea_);
        });
        msg_set_reply_stat(&rmsg, SUCCESS);
        d_.logger_.template log_server<ProcMeta>(rmsg, res);
        return make_record(rmsg, res);
      } catch (const std::exception &e) {
        msg_set_reply_stat(&rmsg, SYSTEM_ERR);
        std::println(stderr, "{}::{}: {}", ProcMeta::version_meta::version_name,
                     ProcMeta::proc_name, e.what());
        if constexpr (!std::same_as<Logger, RpcNoLogger>)
          d_.logger_.template log_server<ProcMeta>(rmsg,
                                                   uformat(" {}", e.what()));
        return make_record(rmsg);
      }
    }
  };
};

template<typename T, typename VersMeta, typename... ExtraArgs>
concept implements_srpc =
    implements_rpc<T, VersMeta, std::type_identity_t, ExtraArgs...>;

template<typename Logger = RpcNoLogger> struct SRpcStream : public SRpcFd {
  using super = SRpcFd;
  static constexpr bool kAllowRefPtrConversion = true;

  Logger logger_;

  SRpcStream(UniqueFd fd, Logger logger = {})
    : SRpcFd(std::move(fd)), logger_(std::move(logger))
  {}

  template<typename Server,
           typename VersMeta = typename std::remove_cvref_t<
               decltype(*std::declval<Server>())>::rpc_service,
           typename... ExtraArgs>
  requires implements_srpc<Server, VersMeta, ExtraArgs...>
  SRpcStream &server(Server srvr, VersMeta = {}, ExtraArgs &&...extra_args)
  {
    auto dispatch = new SRpcDispatcher<VersMeta, Server, Logger,
                                       std::decay_t<ExtraArgs>...>(
        std::move(srvr), logger_, std::forward<ExtraArgs>(extra_args)...);
    srvmap_.emplace(dispatch->getpv(), dispatch);
    return *this;
  }
  template<typename VersMeta, typename Server, typename... ExtraArgs>
  requires implements_srpc<Server, VersMeta, ExtraArgs...>
  SRpcStream &server(Server srvr, ExtraArgs &&...ea)
  {
    return server<Server, VersMeta, ExtraArgs...>(std::move(srvr), {}, ea...);
  }

  using ClientRef = RefPtr<SRpcStream>;

  template<typename VersMeta>
  requires requires { typename VersMeta::template client<ClientRef>; }
  typename VersMeta::template client<ClientRef> client(VersMeta = {})
  {
    return typename VersMeta::template client<ClientRef>{ClientRef{*this}};
  }

  struct Configurator {
    SRpcStream *s_;
    const CpuSet *cpus_{};
    Configurator(SRpcStream *s) : s_(s) {}
    void set_affinity(const CpuSet *cpus) { cpus_ = cpus; }
    SRpcStream *operator->() const { return s_; }
    ~Configurator() { s_->go(cpus_); }
  };

  template<typename ProcMeta>
  ProcMeta::return_type send_call(const is_xdr_type auto &...args)
  {
    auto msg = call_msg<ProcMeta>();
    const bool logging = logger_.active();
    Xid xid = logging ? genxid() : 0;
    msg.xid = xid;
    Buf record = make_record(msg, args...);

    using ErrType = std::variant<XdrBadInput, XdrCallFailed>;
    std::expected<typename ProcMeta::return_type, ErrType> ret;
    std::binary_semaphore done{0};

    write_call(record, [&](rpc_msg *msg, BufView res) {
      if (!msg) {
        logger_.log_client_fail(xid, "EOF");
        ret = std::unexpected{XdrBadInput("EOF")};
      }
      else if (!rpc_is_success(*msg)) {
        logger_.template log_client<ProcMeta>(*msg);
        ret = std::unexpected{XdrCallFailed(std::move(*msg))};
      }
      else
        try {
          if constexpr (std::is_void_v<typename decltype(ret)::value_type>) {
            if (!res.empty())
              throw XdrBadInput("Received bytes for void result");
          }
          else
            deserialize(&ret.value(), res);
        } catch (XdrBadInput &e) {
          if (logging)
            logger_.template log_client<ProcMeta>(
                *msg, uformat("deserializion error: {}", e.what()));
          ret = std::unexpected{std::move(e)};
        }
      done.release();
    });
    logger_.template log_client<ProcMeta>(msg, std::forward_as_tuple(args...));

    done.acquire();

    if (!ret)
      std::visit([&](auto e) { throw e; }, ret.error());
    return *std::move(ret);
  }
};

template<typename Logger = RpcNoLogger>
typename SRpcStream<Logger>::Configurator
srpc_stream(int fd, Logger logger = {})
{
  return new SRpcStream(UniqueFd{fd}, std::move(logger));
}

inline SRpcStream<RpcLogger>::Configurator
srpc_stream_logged(int fd, int logfd = RpcLogger::default_logfd)
{
  return new SRpcStream<RpcLogger>(UniqueFd{fd}, RpcLogger(fd, logfd));
}

template<typename Logger>
struct SRpcMulticlient : RefCount, PerCpuSet<RefPtr<SRpcStream<Logger>>> {
  using super1 = RefCount;
  using super2 = PerCpuSet<RefPtr<SRpcStream<Logger>>>;
  using ClientRef = RefPtr<SRpcMulticlient>;

  template<typename VersMeta>
  requires requires { typename VersMeta::template client<ClientRef>; }
  typename VersMeta::template client<ClientRef> client(VersMeta = {})
  {
    return typename VersMeta::template client<ClientRef>{ClientRef{*this}};
  }

  template<typename ProcMeta>
  ProcMeta::return_type send_call(const is_xdr_type auto &...args)
  {
    return this->lookup()->template send_call<ProcMeta>(args...);
  }

  static ClientRef make(const struct sockaddr_storage &ss,
                        const CpuMap &map = *llc_map,
                        int logfd = RpcLogger::default_logfd)
  {
    return *new SRpcMulticlient(ss, map, logfd);
  }

protected:
  SRpcMulticlient(const struct sockaddr_storage &ss, const CpuMap &map,
                  int logfd)
    : super2(map)
  {
    super2::populate(
        [&ss, logfd](const CpuSet &set) -> SRpcStream<Logger>::ClientRef {
          int fd = tcpconnect(ss).release();
          set_nodelay(fd);
          typename super2::value_type ret =
              *new SRpcStream<Logger>(UniqueFd{fd}, Logger{fd, logfd});
          ret->go(&set);
          return ret;
        });
  }
};

} // namespace x23
