// -*-C++-*-

#pragma once

#include "hogd/base/os.h"
#include <cstddef>

#include <x23/cpumap.h>
#include <x23/debug.h>
#include <x23/event.h>
#include <x23/threadpool.h>

namespace hogd {

inline x23::ThreadPool &
default_threadpool()
{
  static x23::ThreadPool pool{available_parallelism()};
  return pool;
}

inline x23::CoroPoll &
default_coropoll()
{
  static x23::CoroPoll cp{default_threadpool()};
  return cp;
}

} // namespace hogd
