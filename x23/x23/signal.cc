
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
#include <x23/utility.h>

namespace x23 {

#ifndef NSIG
#ifdef _NSIG
#define NSIG _NSIG
#else
#define NSIG 64
#endif
#endif

static_assert(std::atomic<sig_atomic_t>::is_always_lock_free);

namespace {

struct SigState {
  std::atomic<sig_atomic_t> flag{0}; // Set to 1 in signal handler
  unsigned numcb{0}; // Count cbs to reset OS signal handler when 0
};

// Per signal callback state
struct SigCB {
  // The callback to make (if not deleted)
  std::function<void()> cb;

  // True if the callback should be deleted.  Note that we only ever
  // actually destroy callbacks in the unique thread that services
  // them.  Since a callback might want to unregister itself, we just
  // keep a deleted flag that other threads can set.  Then the service
  // thead just destroys the callback next time through.
  std::atomic_bool deleted{false};

  SigCB(SigCB &&other) noexcept
    : cb(std::move(other.cb)), deleted(other.deleted.load())
  {}
  SigCB(std::function<void()> &&c) : cb(std::move(c)) {}
};

// ID of dedicated service thread
std::thread::id sig_service_thread_id;

// Even when the service thread is quiescent, odd when it is making
// callbacks.
std::atomic_uint64_t sig_service_count;

// Per-signal state
std::array<SigState, NSIG> sig_state;

// Table of callbacks.  Allocate with new so it doesn't get destroyed,
// as we may want to keep servicing signal callbacks during global
// destruction, when shutdown is hanging.
MVar<std::multimap<int, SigCB>>::Ptr
take_sig_cbs()
{
  static auto &sig_cbs = make_nodestroy(new MVar<std::multimap<int, SigCB>>);
  return sig_cbs.take();
}

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
  if (sig > 0 && size_t(sig) < sig_state.size())
    sig_state[sig].flag.store(1);
  write(get_sig_pipe()[1], "", 1);
}

void
service_once()
{
  int sig = 0;
  bool happened = false;
  auto ptr = take_sig_cbs();
  auto it = ptr->begin();

  while (it != ptr->end()) {
    if (sig != it->first) {
      sig = it->first;
      happened = sig_state[sig].flag.load();
    }

    if (happened && !it->second.deleted) {
      ptr.reset();
      it->second.cb();
      ptr = take_sig_cbs();
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

void
service_loop()
{
  const int fd = get_sig_pipe()[0];
  for (;;) {
    std::array<char, 64> buf;
    ::read(fd, buf.data(), buf.size());
    sig_service_count.fetch_add(1);
    service_once();
    sig_service_count.fetch_add(1);
    sig_service_count.notify_all();
  }
}

} // namespace

RawSignalCbHandle
signal_cb(int sig, std::function<void()> cb)
{
  assert(sig > 0 && size_t(sig) < sig_state.size());

  static std::atomic_flag init{};
  if (!init.test_and_set(std::memory_order_relaxed)) {
    std::thread service_thread(service_loop);
    sig_service_thread_id = service_thread.get_id();
    service_thread.detach();
  }

  auto ptr = take_sig_cbs();
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

void
RawSignalCbHandle::cancel()
{
  assert(delflag_ && !delflag_->load(std::memory_order_relaxed));
  *delflag_ = true;
  delflag_ = nullptr;

  auto count = sig_service_count.load();
  handler(0); // Ensure service_loop removes cb from map

  // If we are servicing signals, wait to ensure callbacks are done
  // before returning.  Otherwise, if this is happening in a
  // destructor, we could potentially be right about to fire the
  // callback as an object accessed by the callback is destroyed.
  if ((count & 1) && std::this_thread::get_id() != sig_service_thread_id)
    sig_service_count.wait(count);
}

} // namespace x23
