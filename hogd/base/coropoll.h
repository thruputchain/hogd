// -*-C++-*-

#pragma once

#include <x23/event.h>

namespace hogd {

inline x23::ThreadPool &
default_threadpool()
{
  static x23::ThreadPool pool;
  return pool;
}

inline x23::CoroPoll *cp = new x23::CoroPoll{default_threadpool()};

inline x23::CoroPoll &
default_coropoll()
{
  return *cp;
}

// TODO(jhumphri): Can we take this out?
inline void
reconstruct_default_coropoll()
{
  delete cp;
  cp = new x23::CoroPoll{default_threadpool()};
}

} // namespace hogd
