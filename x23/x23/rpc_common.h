// -*-C++-*-

// Common types used by both synchronous and asynchronous coroutines.

#pragma once

#include <map>

#include <x23/format.h>
#include <x23/rpc_msg.hh>
#include <x23/serialize.h>

namespace x23 {

struct XdrCallFailed : XdrException {
  using enum msg_type;
  const rpc_msg m_;
  XdrCallFailed(const rpc_msg &m) : XdrException(make_message(m)), m_(m) {}
  XdrCallFailed(rpc_msg &&m) : XdrException(make_message(m)), m_(std::move(m))
  {}

  static std::string make_message(const rpc_msg &m)
  {
    using enum reply_stat;
    assert(m.body.mtype() == REPLY);
    if (m.body.rbody().stat() == MSG_ACCEPTED) {
      auto stat = m.body.rbody().areply().reply_data.stat();
      return std::format("RPC call failed ({})", stat);
    }
    else {
      auto stat = m.body.rbody().rreply().stat();
      return std::format("RPC call rejected ({})", stat);
    }
  }
};

template<typename ProcMeta>
inline rpc_msg
call_msg()
{
  using enum msg_type;
  using enum auth_flavor;
  rpc_msg msg{};
  msg.body.mtype(CALL);
  msg.body.cbody().rpcvers = 2;
  msg.body.cbody().prog = ProcMeta::version_meta::program;
  msg.body.cbody().vers = ProcMeta::version_meta::version;
  msg.body.cbody().proc = ProcMeta::proc;
  msg.body.cbody().cred.flavor = AUTH_NONE;
  msg.body.cbody().verf.flavor = AUTH_NONE;
  return msg;
}

inline void
msg_set_reply_stat(rpc_msg *msg, accept_stat stat)
{
  using enum msg_type;
  using enum reply_stat;
  msg->body.mtype(REPLY);
  msg->body.rbody().stat(MSG_ACCEPTED);
  msg->body.rbody().areply().reply_data.stat(stat);
}
inline void
msg_set_reply_stat(rpc_msg *msg, reject_stat stat)
{
  using enum reply_stat;
  using enum reject_stat;
  using enum msg_type;
  msg->body.mtype(REPLY);
  msg->body.rbody().stat(MSG_DENIED);
  msg->body.rbody().rreply().stat(stat);
  if (stat == RPC_MISMATCH)
    msg->body.rbody().rreply().mismatch_info().low =
        msg->body.rbody().rreply().mismatch_info().high = 2;
}
inline void
msg_set_reply_stat(rpc_msg *msg, auth_stat stat)
{
  using enum reject_stat;
  msg_set_reply_stat(msg, AUTH_ERROR);
  msg->body.rbody().rreply().rj_why() = stat;
}

std::string msg_show_stat(const rpc_msg &msg);

Buf
make_record(const rpc_msg &msg, const is_xdr_type auto &...x)
{
  Buf ret = serialize(std::uint32_t{}, msg, x...);
  assert(ret.size() <= 0x80000003UZ);
  putbe<std::uint32_t>(ret.data(), 0x80000000 | (ret.size() - 4));
  return ret;
}

inline bool
rpc_is_success(const rpc_msg &m)
{
  using enum accept_stat;
  using enum reply_stat;
  using enum msg_type;
  return m.body.mtype() == REPLY && m.body.rbody().stat() == MSG_ACCEPTED &&
         m.body.rbody().areply().reply_data.stat() == SUCCESS;
}

struct ProgVers {
  std::uint32_t prog;
  std::uint32_t vers;
  friend bool operator==(ProgVers, ProgVers) = default;
  friend std::strong_ordering operator<=>(ProgVers, ProgVers) = default;
};

template<typename T>
struct SrvMap : public std::map<ProgVers, std::unique_ptr<T>> {
  using super = std::map<ProgVers, std::unique_ptr<T>>;
  T *lookup(const rpc_msg &msg, rpc_msg *err) const
  {
    using enum accept_stat;
    using enum reject_stat;
    err->xid = msg.xid;
    if (msg.body.cbody().rpcvers != 2) [[unlikely]] {
      msg_set_reply_stat(err, RPC_MISMATCH);
      return nullptr;
    }

    ProgVers pv{.prog = msg.body.cbody().prog, .vers = msg.body.cbody().vers};
    auto it = super::find(pv);
    if (it != super::end())
      return it->second.get();

    it = super::lower_bound(ProgVers{.prog = pv.prog, .vers = 0});
    if (it == super::end() || it->first.prog != pv.prog)
      msg_set_reply_stat(err, PROG_UNAVAIL);
    else {
      msg_set_reply_stat(err, PROG_MISMATCH);
      auto &mmi = err->body.rbody().areply().reply_data.mismatch_info();
      mmi.low = it->first.vers;
      it = super::upper_bound(ProgVers{.prog = pv.prog, .vers = 0xffffffff});
      mmi.high = (--it)->first.vers;
    }
    return nullptr;
  }
  T *lookup(rpc_msg *msgerr) const { return lookup(*msgerr, msgerr); }
};

// Invoke f<Vers::proc_meta<procno>> if such a type exists, otherwise
// invoke f<void>.
template<typename Vers, typename R = void, typename F>
R
with_proc_meta(std::uint32_t procno, F &&f)
{
  return [procno,
          &f]<size_t... I>(std::index_sequence<I...>) -> decltype(auto) {
    static const auto invokers = std::unordered_map{std::pair{
        Vers::valid_procnos[I], +[](F &&f) -> R {
          return std::forward<F>(f)
              .template operator()<
                  typename Vers::template proc_meta<Vers::valid_procnos[I]>>();
        }}...};
    if (auto it = invokers.find(procno); it != invokers.end())
      return it->second(std::forward<F>(f));
    return std::forward<F>(f).template operator()<void>();
  }(std::make_index_sequence<Vers::valid_procnos.size()>{});
}

struct RpcLogger {
  static inline int default_logfd = -1;
  int logfd_ = default_logfd;
  std::string peername_;

  RpcLogger() noexcept = default;
  RpcLogger(std::string peername, int logfd = default_logfd) noexcept
    : logfd_(logfd), peername_(std::move(peername))
  {}
  RpcLogger(int peerfd, int logfd = default_logfd) noexcept;
  bool active() const { return logfd_ >= 0; }

  template<typename Meta = void>
  static std::string
  make_log_message(bool am_server, const std::string &peername,
                   const rpc_msg &msg, const is_xdr_type auto &...pl)
      requires (sizeof...(pl) <= 1)
  {
    using enum msg_type;
    std::string ret =
        std::format("{} {} xid={} {}", peername,
                    am_server == (msg.body.mtype() == CALL) ? "==>" : "<==",
                    msg.xid, msg.body.mtype());
    if (msg.body.mtype() == CALL) {
      if constexpr (requires {
                      { Meta::proc_name } -> decays_to<const char *>;
                    })
        uformat_to(&ret, " {}::{}", Meta::version_meta::version_name,
                   Meta::proc_name);
      else if constexpr (requires {
                           { Meta::version_name } -> decays_to<const char *>;
                         })
        uformat_to(&ret, " {}::<proc#{}>", Meta::version_name,
                   msg.body.cbody().proc);
      else
        uformat_to(&ret, " <prog#{},vers#{}>::<proc#{}>", msg.body.cbody().prog,
                   msg.body.cbody().vers, msg.body.cbody().proc);
    }
    else if (!rpc_is_success(msg))
      uformat_to(&ret, " FAIL {}", msg_show_stat(msg));
    if constexpr (sizeof...(pl) > 0) {
      ret += ' ';
      ret = show(pl..., 0, std::move(ret));
    }
    if (ret.back() != '\n')
      ret += '\n';
    return ret;
  }

  template<typename Meta = void>
  void log_client(const rpc_msg &msg, const is_xdr_type auto &...pl)
      requires (sizeof...(pl) <= 1)
  {
    if (logfd_ < 0)
      return;
    auto logmsg = make_log_message<Meta>(false, peername_, msg, pl...);
    write(logfd_, logmsg.data(), logmsg.size());
  }
  void log_client_fail(std::uint32_t xid, std::string_view text)
  {
    if (logfd_ < 0)
      return;
    auto logmsg = std::format("{} CALL xid={} ==> {}\n", peername_, xid, text);
    write(logfd_, logmsg.data(), logmsg.size());
  }
  template<typename Meta = void>
  void log_server(const rpc_msg &msg, const is_xdr_type auto &...pl)
      requires (sizeof...(pl) <= 1)
  {
    if (logfd_ < 0)
      return;
    auto logmsg = make_log_message<Meta>(true, peername_, msg, pl...);
    write(logfd_, logmsg.data(), logmsg.size());
  }
};

struct RpcNoLogger {
  RpcNoLogger() noexcept = default;
  RpcNoLogger(const std::string &, int = RpcLogger::default_logfd) noexcept {}
  RpcNoLogger(int, int = RpcLogger::default_logfd) noexcept {}
  static constexpr bool active() { return false; }
  template<typename = void>
  void log_client(const rpc_msg &msg, const is_xdr_type auto &...pl)
      requires (sizeof...(pl) <= 1)
  {}
  void log_client_fail(std::uint32_t xid, std::string_view text) {}
  template<typename = void>
  void log_server(const rpc_msg &msg, const is_xdr_type auto &...pl)
      requires (sizeof...(pl) <= 1)
  {}
};

} // namespace x23
