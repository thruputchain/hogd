// -*-C++-*-

#pragma once

#include <x23/event.h>
#include <x23/threadpool.h>

namespace hogd {

inline x23::ThreadPool &
default_threadpool()
{
  static x23::ThreadPool pool;
  return pool;
}

inline x23::CoroPoll &
default_coropoll()
{
  static x23::CoroPoll cp{default_threadpool()};
  return cp;
}

} // namespace hogd
