
#include "x23/corosync.h"
#include "x23/threadpool.h"

namespace x23 {

namespace detail {

void
resume_in_another_thread(std::coroutine_handle<> h)
{
  if (ThreadPool *pool = ThreadPool::get_thread_pool())
    pool->go(h);
  else
    h();
}

} // namespace detail

} // namespace x23
