#include "x23/event.h"

#include <algorithm>
#include <cassert>
#include <cerrno>
#include <memory>
#include <ranges>
#include <sys/poll.h>
#include <unistd.h>
#include <utility>
#include <vector>

#include "x23/cleanup.h"
#include "x23/err.h"
#include "x23/sockio.h"
#include "x23/threadpool.h"

namespace x23::detail {

CoroPollImpl::CoroPollImpl(ThreadPool &pool_) : pool_(pool_)
{
  int fds[2];
  if (pipe(fds))
    Errno{}.raise("pipe");
  for (int i = 0; i < 2; ++i)
    selfpipe_[i].reset(fds[i]);
  // Don't combine loops; set_nonblock might throw.
  for (int i = 0; i < 2; ++i)
    set_nonblock(selfpipe_[i].get());
}

CoroPollImpl::~CoroPollImpl()
{}

bool
CoroPollImpl::time_to_stop()
{
  if (!stop_requested() || !state_->empty())
    return false;
  pool_.drain();
  return state_->empty();
}

RefPtr<ReadBuffer>
CoroPollImpl::get_reader(BorrowedFd fd)
{
  auto s = state_.take();
  auto &f = (*s)[fd.get()];
  if (f.poisoned)
    return nullptr;
  return f.reader();
}

RefPtr<WriteBuffer>
CoroPollImpl::get_writer(BorrowedFd fd)
{
  auto s = state_.take();
  auto &f = (*s)[fd.get()];
  if (f.poisoned)
    return nullptr;
  return f.writer();
}

std::pair<RefPtr<ReadBuffer>, RefPtr<WriteBuffer>>
CoroPollImpl::get_rw(BorrowedFd fd)
{
  auto s = state_.take();
  auto &f = (*s)[fd.get()];
  if (f.poisoned)
    return {nullptr, nullptr};
  return {f.reader(), f.writer()};
}

void
CoroPollImpl::check(int timeout)
{
  std::vector<struct pollfd> pollfds;
  {
    auto ptr = state_.take();
    pollfds.reserve(ptr->size() + 1);
    pollfds.push_back({selfpipe_[0].get(), pollflags[0], 0});
    for (const auto &kv : *ptr) {
      const FdState &f = kv.second;
      short events;
      if (f.poisoned || !(events = f.events()))
        continue;
      pollfds.push_back({kv.first, events, 0});
    }
  }

  int n = poll(pollfds.data(), pollfds.size(), timeout);
  if (n <= 0) {
    if (n < 0 && errno != EINTR)
      Errno{}.raise("poll");
    return;
  }
  if (pollfds[0].revents) {
    char buf[64];
    while (::read(selfpipe_[0].get(), &buf, sizeof(buf)) == sizeof(buf))
      ;
    if (!--n)
      return;
  }

  auto ptr = state_.take();
  std::vector<ThreadPool::Job> cbs;
  for (auto pfd = pollfds.begin() + 1; pfd != pollfds.end(); ++pfd) {
    auto it = ptr->find(pfd->fd);
    if (it == ptr->end())
      continue;
    FdState &f = it->second;
    if (f.poisoned)
      continue;
    for (int write = 0; write < 2; ++write)
      if (f.callbacks[write] && (pfd->revents & pollflags[write]))
        if (auto h = std::exchange(f.callbacks[write].value, nullptr)
                         .set_result_without_resuming(0))
          cbs.push_back({h, f.callbacks[write].cpuhint});
  }
  ptr.reset();
  pool_.go(std::move(cbs));
}

int
CoroPollImpl::get_max_fd()
{
  auto ptr = state_.take();
  if (ptr->empty())
    return 0;
  return 1 + std::ranges::max(*ptr | std::views::keys);
}

std::vector<ThreadPool::Job>
CoroPollImpl::report_badfd(FdState &f)
{
  std::vector<ThreadPool::Job> ret;
  for (unsigned write = 0; write < 2; ++write)
    if (f.callbacks[write])
      if (auto h = std::exchange(f.callbacks[write].value, nullptr)
                       .set_result_without_resuming(EBADF))
        ret.push_back({h, f.callbacks[write].cpuhint});
  return ret;
}

void
CoroPollImpl::poison(BorrowedFd fd)
{
  auto s = state_.take();
  auto &f = (*s)[fd.get()];
  f.poisoned = true;
  auto work = report_badfd(f);
  s.reset();
  pool_.go(std::move(work));
}

void
CoroPollImpl::reset(BorrowedFd fd)
{
  auto ptr = state_.take();
  if (auto it = ptr->find(fd.get()); it != ptr->end()) {
    auto work = report_badfd(it->second);
    ptr->erase(it);
    // Note we must always wake, even when the events are not empty
    // and no stop is requested.  The reason is that poll keeps a
    // reference to sockets, which means closing a file descriptor
    // will not close the socket (and hence not send a TCP FIN
    // segment) while poll is still waiting for an event.  Waking the
    // poll thread ensures closed file descriptors also close sockets.
    wake();
    ptr.reset();
    pool_.go(std::move(work));
  }
}

void
CoroPollImpl::clear(BorrowedFd fd, bool)
{}

void
CoroPollImpl::setcb(BorrowedFd fd, bool write, WorkItem cb)
{
  if (stop_requested()) {
    pool_.make_callback(cb, EBADF);
    return;
  }
  auto ptr = state_.take();
  auto &f = (*ptr)[fd.get()];
  assert(!f.callbacks[write]);
  if (f.poisoned) {
    pool_.make_callback(cb, EBADF);
    return;
  }
  f.callbacks[write] = cb;
  wake();
}

void
CoroPollImpl::wake()
{
  static const char z = '\0';
  write(selfpipe_[1].get(), &z, 1);
}

void
CoroPollImpl::abort(BorrowedFd fd, bool write, int err)
{
  ThreadPool::Job h{};
  {
    auto ptr = state_.take();
    auto &f = (*ptr)[fd.get()];
    if (f.callbacks[write])
      h = {std::exchange(f.callbacks[write], {})
               ->set_result_without_resuming(err),
           f.callbacks[write].cpuhint};
  }
  if (h)
    pool_.go(h);
}

} // namespace x23::detail
