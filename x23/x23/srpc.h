// -*-C++-*-

#pragma once

#include <condition_variable>
#include <print>
#include <stop_token>
#include <unordered_map>

#include <x23/cleanup.h>
#include <x23/mvar.h>
#include <x23/rpc_common.h>
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

class SRpcFd {
public:
  using CallCB = std::function<void(BufView)>;

  SRpcFd(UniqueFd fd);
  ~SRpcFd();

private:
  using Xid = std::uint32_t;

  UniqueFd fd_;
  VecQ<std::uint8_t> rdbuf_;
  BufVec wrbuf_;

  //std::unordered_map<ProgVers, std::unique_ptr<SRpcService>> services_;

  Xid lastxid_{};
  MVar<std::unordered_map<Xid, CallCB>> pending_calls_;

  struct WriteItem {
    Buf data_;
    CallCB replycb_;
  };
  MVar<Vec<WriteItem>> write_queue_;
  std::condition_variable write_ready_;

  std::stop_source ss_;

  void read_worker();
  void write_worker();
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
        auto res = void_to_tuple([&] {
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

} // namespace x23
