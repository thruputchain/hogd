
#include <array>
#include <cassert>
#include <map>
#include <thread>

#include <fcntl.h>
#include <signal.h> // Need for NSIG (might not be in <csignal>)
#include <unistd.h>

#include <x23/mvar.h>
#include <x23/signal.h>
#include <x23/sockio.h>

namespace x23 {

#ifndef NSIG
#ifdef _NSIG
#define NSIG _NSIG
#else
#define NSIG 64 // fallback if you must
#endif
#endif

static_assert(NSIG < 0x1000);
static_assert(std::atomic<sig_atomic_t>::is_always_lock_free);

namespace {

struct SigState {
  std::atomic<sig_atomic_t> flag{}; // Set to 1 in signal handler
  unsigned numcb{0}; // Count cbs to reset OS signal handler when 0
};

struct SigCB {
  std::function<void()> cb;        // The callback to make (if not deleted)
  std::atomic_bool deleted{false}; // True if callback deleted

  SigCB(SigCB &&other) noexcept
    : cb(std::move(other.cb)), deleted(other.deleted.load())
  {}
  SigCB(std::function<void()> &&c) : cb(std::move(c)) {}
};

// Per-signal state
std::array<SigState, NSIG> sig_state;

// Table of callbacks.  Allocate with new so it doesn't get destroyed,
// as we may want to keep servicing signal callbacks during global
// destruction, when shutdown is hanging.
auto &sig_cbs = *new MVar<std::multimap<int, SigCB>>;

// We wake up the service loop by writing to a pipe.  This function
// allocates the pipe the first time it is invoked.
const std::array<int, 2> &
get_sig_pipe()
{
  static const auto fds = [] {
    std::array<int, 2> fds{-1, -1};
    if (pipe(fds.data()))
      Errno{}.raise("pipe");
    for (const auto &f : fds)
      fcntl(f, F_SETFD, 1);
    set_nonblock(fds[1]);
    return fds;
  }();
  return fds;
}

void
handler(int sig)
{
  assert(size_t(sig) < sig_state.size());
  sig_state[sig].flag.store(1);
  write(get_sig_pipe()[1], "", 1);
}

void
service_once()
{
  for (int sig = 0; size_t(sig) < sig_state.size(); ++sig) {
    if (!sig_state[sig].flag.load() || !sig_state[sig].flag.exchange(0))
      continue;
    auto ptr = sig_cbs.take();
    auto it = ptr->lower_bound(sig);
    while (it != ptr->end() and it->first == sig) {
      if (!it->second.deleted) {
        auto &cb = it->second.cb;
        ptr.reset();
        cb();
        ptr = sig_cbs.take();
      }
      if (it->second.deleted) {
        it = ptr->erase(it);
        if (!--sig_state[sig].numcb)
          signal(sig, SIG_DFL);
      }
      else
        ++it;
    }
  }
}

void
service_loop()
{
  const int fd = get_sig_pipe()[0];
  for (;;) {
    std::array<char, 64> buf;
    ::read(fd, buf.data(), buf.size());
    service_once();
  }
}

} // namespace

RawSignalCbHandle
signal_cb(int sig, std::function<void()> cb)
{
  assert(sig > 0 && size_t(sig) < sig_state.size());

  static std::atomic_flag init{};
  if (!init.test_and_set(std::memory_order_relaxed))
    std::thread(service_loop).detach();

  auto ptr = sig_cbs.take();
  auto it = ptr->emplace(sig, std::move(cb));
  if (!sig_state[sig].numcb++) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handler;
    sa.sa_flags = SA_RESTART;
    if (sigaction(sig, &sa, nullptr) == -1)
      Errno{}.raise("sigaction({})", sig);
  }
  return &it->second.deleted;
}

} // namespace x23
