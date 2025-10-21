
#include "x23/corotrace.h"
#include "x23/vec.h"

namespace x23 {

std::string
trace(const PromiseBase *pb, unsigned indent, std::string ret)
{
#if X23_CORO_TRACE
  for (; pb; pb = pb->waiter())
    uformat_to(&ret,
               "{0:>{1}}{2}\n"
               "{0:>{1}}  created at {3}:{4}\n"
               "{0:>{1}}  suspended at {5}:{6}\n",
               "", indent, pb->suspension_.function_name(),
               pb->creation_.file_name(), pb->creation_.line(),
               pb->suspension_.file_name(), pb->suspension_.line());
#endif // X23_CORO_TRACE
  return ret;
}

} // namespace x23
