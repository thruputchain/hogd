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
//   c->null();  // makes remote procedure call
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
// EOF or error from the file descriptor.

#pragma once

#include <atomic>
#include <concepts>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <expected>
#include <format>
#include <functional>
#include <print>
#include <semaphore>
#include <stop_token>
#include <sys/socket.h>
#include <thread>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <variant>

#include <x23/cleanup.h>
#include <x23/cpumap.h>
#include <x23/debug.h>
#include <x23/mvar.h>
#include <x23/refcnt.h>
#include <x23/rpc_common.h> // IWYU pragma: export
#include <x23/rpc_msg.hh>
#include <x23/serialize.h>
#include <x23/sockio.h>
#include <x23/uiobuf.h>
#include <x23/vec.h>
#include <x23/x23.h>

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
  static inline std::atomic<bool> ignore_sigpipe_{true};

  // Note the BufView argument to CallCB is only the arguments, not
  // the rpc_msg, which has already been decoded.  (This is an
  // asymmetry with write_call, in which the Buf argument contains the
  // record length, rpc_msg, and arguments.)
  using CallCB = std::function<void(rpc_msg *, BufView)>;

  // Note don't set this larger than 0x7fffffff because we don't
  // support chunked messages.
  size_t max_message_size_;
  SrvMap<SRpcService> srvmap_{};

  SRpcFd(UniqueFd fd);
  SRpcFd(const SRpcFd &) = delete;

  template<typename... Args>
  void fail(std::format_string<Args...> fmt, Args &&...args) noexcept
  {
    Str msg = uformat("SRpcFd(fd {}): ", fd_.get()) +
              uvformat(fmt.get(), std::make_format_args(args...));
    // NOLINTNEXTLINE(readability-static-accessed-through-instance)
    debug.err("{}", msg);
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
    x23assert(msg.body.mtype() == CALL && msg.body.cbody().rpcvers == 2 &&
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
      } catch (const std::exception &e) {
        // Argument decode can throw exceptions other than XdrException --
        // notably std::bad_alloc when an unbounded vector of zero-wire-size
        // elements is decoded with a huge count.  Convert them to SYSTEM_ERR
        // rather than letting them cross the noexcept SRpcFd::input boundary
        // and terminate the process.
        msg_set_reply_stat(&rmsg, SYSTEM_ERR);
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
                return ProcMeta::dispatch(*d_.impl_, std::move(args), ea...);
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
               decltype(*declval<Server>())>::rpc_service,
           typename... ExtraArgs>
  requires implements_srpc<Server, VersMeta, ExtraArgs...>
  SRpcStream &server(Server srvr, VersMeta /*unused*/ = {},
                     ExtraArgs &&...extra_args)
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
  typename VersMeta::template client<ClientRef> client(VersMeta /*unused*/ = {})
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
  ProcMeta::return_type send_call(const is_xdr_serializable auto &...args)
  {
    // If this fails, you tried to make a blocking RPC call from
    // within a coroutine, which can cause deadlock.
    extern bool in_thread_pool();
    x23assert(!in_thread_pool());

    auto msg = call_msg<ProcMeta>();
    const bool logging = logger_.active();
    Xid xid = logging ? genxid() : 0;
    msg.xid = xid;
    Buf record = make_record(msg, args...);

    if (record.size() - 4 > max_message_size_) [[unlikely]]
      throw XdrBadInput("{}::{}: message size {} exceeds maximum {}",
                        ProcMeta::version_meta::version_name,
                        ProcMeta::proc_name, record.size() - 4,
                        max_message_size_);

    using ErrType = std::variant<XdrException, XdrCallFailed>;
    std::expected<typename ProcMeta::return_type, ErrType> ret;
    std::binary_semaphore done{0};

    write_call(record, [&](rpc_msg *msg, BufView res) {
      if (!msg) {
        auto err = std::format("EOF waiting for result of {}::{}",
                               ProcMeta::version_meta::version_name,
                               ProcMeta::proc_name);
        logger_.log_client_fail(xid, err);
        ret = std::unexpected{XdrException(err)};
      }
      else if (!rpc_is_success(*msg)) {
        logger_.template log_client<ProcMeta>(*msg);
        XdrCallFailed err(std::move(*msg));
        err.what_ =
            std::format("{}::{}: {}", ProcMeta::version_meta::version_name,
                        ProcMeta::proc_name, err.what_);
        ret = std::unexpected{std::move(err)};
      }
      else
        try {
          if constexpr (std::is_void_v<typename decltype(ret)::value_type>) {
            if (!res.empty())
              throw XdrBadInput("Received bytes for void result of {}::{}",
                                ProcMeta::version_meta::version_name,
                                ProcMeta::proc_name);
          }
          else {
            deserialize(&ret.value(), res);
            logger_.template log_client<ProcMeta>(*msg, ret.value());
          }
        } catch (XdrException &e) {
          // Catch the XdrException base, not just XdrBadInput: a malformed
          // reply body can also throw XdrLengthError (e.g. an oversized
          // bounded vector count), and letting a sibling escape this callback
          // would cross the noexcept boundary in SRpcFd::input and call
          // std::terminate.  Hand the caught exception back to the blocked
          // caller as-is; callers catch the generic XdrException base.
          if (logging)
            logger_.template log_client<ProcMeta>(
                *msg, uformat("deserializion error: {}", e.what()));
          e.what_ =
              std::format("{}::{}: {}", ProcMeta::version_meta::version_name,
                          ProcMeta::proc_name, e.what_);
          ret = std::unexpected{std::move(e)};
        } catch (const std::exception &e) {
          // A non-XdrException here (notably std::bad_alloc from decoding a
          // reply that contains an unbounded vector of zero-wire-size
          // elements) must likewise not escape this callback and cross the
          // noexcept SRpcFd::input boundary.  Surface it to the caller as an
          // XdrException, symmetric with the SRPC server argument-decode path.
          if (logging)
            logger_.template log_client<ProcMeta>(
                *msg, uformat("deserializion error: {}", e.what()));
          ret = std::unexpected{XdrException(
              std::format("{}::{}: {}", ProcMeta::version_meta::version_name,
                          ProcMeta::proc_name, e.what()))};
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

template<typename Logger = RpcLogger>
inline SRpcStream<Logger>::Configurator
srpc_stream_logged(int fd, int logfd = RpcLogger::default_logfd)
{
  return new SRpcStream<Logger>(UniqueFd{fd}, Logger(fd, logfd));
}

template<typename Logger>
struct SRpcMulticlient : RefCount, PerCpuSet<RefPtr<SRpcStream<Logger>>, true> {
  using super1 = RefCount;
  using super2 = PerCpuSet<RefPtr<SRpcStream<Logger>>, true>;
  using ClientRef = RefPtr<SRpcMulticlient>;

  template<typename VersMeta>
  requires requires { typename VersMeta::template client<ClientRef>; }
  typename VersMeta::template client<ClientRef> client(VersMeta /*unused*/ = {})
  {
    return typename VersMeta::template client<ClientRef>{ClientRef{*this}};
  }

  template<typename ProcMeta>
  ProcMeta::return_type send_call(const is_xdr_serializable auto &...args)
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
