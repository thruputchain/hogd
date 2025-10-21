
#include "x23/corotrace.h"
#include "x23/vec.h"

namespace x23 {

std::string
trace(const PromiseBase *pb, unsigned indent, std::string ret)
{
#if X23_CORO_TRACE
  for (PromiseBase *next; pb; pb = next) {
    uformat_to(&ret,
               "{0:>{1}}{2}\n"
               "{0:>{1}}  suspended at {3}:{4}\n",
               "", indent, pb->suspension_.function_name(),
               pb->suspension_.file_name(), pb->suspension_.line());
    next = pb->waiter_;
    if (!next)
      uformat_to(&ret, "{0:>{1}}  created at {2}:{3}\n", "", indent,
                 pb->creation_.file_name(), pb->creation_.line());
  }
#endif // X23_CORO_TRACE
  return ret;
}

} // namespace x23
