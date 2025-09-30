#include <map>
#include <print>

#include <netdb.h>

#include "x23/rpc_common.h"
#include "x23/sockio.h"

namespace x23 {

size_t default_max_message_size = 0x100'0000;

int RpcLogger::default_logfd = -1;

static const auto _init = ([] {
  if (RpcLogger::default_logfd == -1)
    if (char *p = getenv("X23TRACE"))
      if (int n = atoi(p))
	RpcLogger::default_logfd = n;
 }(), std::tuple{});

RpcLogger::RpcLogger(int peerfd, int logfd) noexcept : logfd_(logfd)
{
  peername_ = std::format("{} (fd {})", show_addr(remote_addr(peerfd)), peerfd);
}

RpcCsvLogger::RpcCsvLogger(int peerfd, int logfd) : logfd_(logfd)
{
  if (logfd_ < 0)
    return;
  srcdest_ = ",";
  for (auto getter : {remote_addr, local_addr}) {
    char name[NI_MAXHOST];
    sockaddr_storage ss = getter(peerfd);
    int err;
    if (is_loopback(ss)) {
      name[sizeof(name) - 1] = '\0';
      err = gethostname(name, sizeof(name) - 1);
    }
    else
      err =
          getnameinfo(reinterpret_cast<const sockaddr *>(&ss), sockaddr_len(ss),
                      name, sizeof(name), nullptr, 0, NI_NOFQDN | NI_NAMEREQD);
    if (err)
      uformat_to(&srcdest_, "{},", HostPort{ss});
    else
      uformat_to(&srcdest_, "{}/{},", auto{name}, HostPort{ss});
  }
}

bool RpcTraceMode::disable_env{};

std::string
msg_show_stat(const rpc_msg &msg)
{
  using enum accept_stat;
  using enum reject_stat;
  using enum msg_type;
  using enum reply_stat;
  if (msg.body.mtype() != REPLY) [[unlikely]]
    return "CALL";
  if (msg.body.rbody().stat() == MSG_ACCEPTED) [[likely]] {
    if (accept_stat stat = msg.body.rbody().areply().reply_data.stat();
        stat == PROG_MISMATCH)
      return std::format(
          "PROG_MISMATCH[low={},high={}]",
          msg.body.rbody().areply().reply_data.mismatch_info().low,
          msg.body.rbody().areply().reply_data.mismatch_info().high);
    else
      return std::format("{}", stat);
  }
  else if (reject_stat stat = msg.body.rbody().rreply().stat();
           stat == AUTH_ERROR)
    return std::format("{}", msg.body.rbody().rreply().rj_why());
  else
    return std::format("{}", stat);
}

std::string
replace_percent(std::string target, std::map<char, std::string> subst)
{
  std::string out;
  for (size_t i = 0, e = target.size(); i < e; ++i) {
    char c = target[i];
    if (c != '%' || i + 1 == e) {
      out.push_back(c);
      continue;
    }
    c = target[++i];
    if (auto s = subst.find(c); s != subst.end()) {
      out += s->second;
    }
    else {
      if (c != '%')
        out.push_back('%');
      out.push_back(c);
    }
  }
  return out;
}

RpcTraceMode
RpcTraceMode::getenv(const char *env_var)
{
  if (disable_env)
    return {};

  const char *p{};
  const auto &pn = getprogname();
  if (!pn.empty()) {
    auto env = std::format("{}_{}", env_var, pn);
    p = ::getenv(env.c_str());
  }
  if (!p)
    p = ::getenv(env_var);
  if (!p || !*p)
    return {};

  bool csv = false;
  if (*p == ',') {
    csv = true;
    ++p;
  }

  int fd{-1};
  if (*p == '.' || *p == '/') {
    auto path =
        replace_percent(p, {{'f', pn}, {'p', std::to_string(getpid())}});
    fd = open(path.c_str(), O_CREAT | O_WRONLY | O_APPEND, 0666);
    if (fd == -1) {
      perror(path.c_str());
      return {};
    }
  }
  else {
    char *e;
    fd = std::strtol(p, &e, 10);
    if (p == e || *e) {
      std::println(stderr, "Ignoring invalid {} value", env_var);
      return {};
    }
  }

  return RpcTraceMode{.mode = csv ? kCsv : kTrace, .fd = fd};
}

RpcTraceMode &
server_trace()
{
  static RpcTraceMode m = RpcTraceMode::getenv("X23SERVER_TRACE");
  return m;
}

} // namespace x23
