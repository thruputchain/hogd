
#include "x23/corosync.h"
#include "x23/corotrace.h"
#include "x23/threadpool.h"

namespace x23 {

namespace detail {

void
resume_in_another_thread(CoroHandle h, ThreadPool *pool)
{
  if (pool)
    pool->go(h);
  else
    h();
}

} // namespace detail

} // namespace x23
