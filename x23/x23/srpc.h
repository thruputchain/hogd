// -*-C++-*-

#pragma once

#include <condition_variable>
#include <memory>
#include <print>
#include <semaphore>
#include <stop_token>
#include <thread>

#include <x23/cleanup.h>
#include <x23/mvar.h>
#include <x23/rpc_common.h>
#include <x23/uiobuf.h>
#include <x23/vec.h>

namespace x23 {

struct Foo {
  int bar(int);
};

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

class SRpcFd : std::enable_shared_from_this<SRpcFd> {
public:
  // Note the BufView argument to CallCB is only the argument, not the
  // rpc_msg, which has already been decoded.
  using CallCB = std::function<void(rpc_msg *, BufView)>;

  // Note the maximum message size is 0x7fffffff because we don't
  // support chunked messages.
  static inline std::uint32_t default_max_message_size = 0x200000;
  size_t max_message_size_;
  SrvMap<SRpcService> srvmap_{};

  SRpcFd(UniqueFd fd);
  SRpcFd(const SRpcFd &) = delete;
  ~SRpcFd();
  void fail();

  // Enqueue a serialized call. record must be a complete RPC record
  // including length, but the XID doesn't matter as it will be
  // overwritten with a unique value at the time the RPC is sent.
  void write_call(Buf record, CallCB cb);

  void go();

private:
  static constexpr size_t kReadSize = 0x10000;
  using Xid = std::uint32_t;

  UniqueFd fd_;

  Xid lastxid_{};
  MVar<std::unordered_map<Xid, CallCB>> pending_calls_;

  struct WriteItem {
    Buf data_;
    CallCB replycb_;
  };
  MVar<Vec<WriteItem>> write_queue_;
  std::condition_variable write_ready_;

  std::stop_source ss_;

  std::thread read_thread_;
  std::thread write_thread_;

  void read_worker();
  void input(BufView);
  void write_worker();
  void write_ready()
  {
    auto _ptr = pending_calls_.take();
    write_ready_.notify_one();
  }
};

namespace detail {
inline auto
void_to_tuple(auto &&f)
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
        auto res = detail::void_to_tuple([&] {
          return std::apply(std::bind_front(ProcMeta::dispatch, *d_.impl_,
                                            args),
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

  /*
  using ClientRef = std::shared_ptr<SRpcStream>;

  template<typename VersMeta>
  requires requires { typename VersMeta::template client<SRpcStream>; }
  typename VersMeta::template client<ClientRef> client(VersMeta = {})
  {
    return typename VersMeta::template client<ClientRef>{ClientRef{*s_}};
  }
  */

  template<typename ProcMeta>
  ProcMeta::return_type send_call(const is_xdr_type auto &...args)
  {
    auto msg = call_msg<ProcMeta>();
    Buf record = make_record(msg, args...);

    std::binary_semaphore done{0};
    std::expected<typename ProcMeta::return_type,
                  std::variant<XdrCallFailed, XdrBadInput>>
        ret;

    write_call(record, [&](rpc_msg *msg, BufView res) {
      if (!msg)
        ret = std::unexpected{XdrBadInput("EOF")};
      if (!rpc_is_success(*msg))
        ret = std::unexpected{XdrCallFailed{*msg}};
      else
        try {
          deserialize(&*ret, res);
        } catch (XdrBadInput &e) {
          ret = std::unexpected{std::move(e)};
        }
      done.release();
    });

    done.acquire();
    if (ret)
      return ret.value();
    std::visit([](auto e) { throw e; }, ret.error());
  }
};

} // namespace x23
