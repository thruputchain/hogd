// large_stack_thread.h - jthread-compatible thread class with configurable
// stack size
//
// On macOS, the default pthread stack size is too small for WebAssembly
// compilation. This class provides a drop-in replacement for std::jthread that
// uses pthread APIs to set a larger stack size.

#pragma once

#include "hogd/base/log.h"
#include "x23/cleanup.h"
#include <cassert>
#include <concepts>
#include <cstddef>
#include <functional>
#include <pthread.h>
#include <stop_token>
#include <type_traits>
#include <utility>

namespace hogd {

// LargeStackThread provides a jthread-compatible API but uses pthread to set
// a larger stack size. This is necessary on macOS where the default stack is
// too small for contract compilation.
class LargeStackThread {
public:
  // Default stack size: 8MB (vs macOS default of ~512KB)
  static constexpr size_t kDefaultStackSize = 8 * 1024 * 1024;

  LargeStackThread() noexcept : thread_(), valid_(false) {}

  // Constructor that takes a callable and its arguments, matching jthread API
  template<typename Func, typename... Args>
  requires std::invocable<std::decay_t<Func>, std::stop_token,
                          std::decay_t<Args>...>
  explicit LargeStackThread(Func &&func, Args &&...args) : valid_(false)
  {
    start_thread(kDefaultStackSize, std::forward<Func>(func),
                 std::forward<Args>(args)...);
  }

  // Constructor with custom stack size
  template<typename Func, typename... Args>
  requires std::invocable<std::decay_t<Func>, std::stop_token,
                          std::decay_t<Args>...>
  LargeStackThread(size_t stack_size, Func &&func, Args &&...args)
    : valid_(false)
  {
    start_thread(stack_size, std::forward<Func>(func),
                 std::forward<Args>(args)...);
  }

  ~LargeStackThread()
  {
    if (valid_) {
      request_stop();
      pthread_join(thread_, nullptr);
    }
  }

  // Disable copy
  LargeStackThread(const LargeStackThread &) = delete;
  LargeStackThread &operator=(const LargeStackThread &) = delete;

  // Move constructor
  LargeStackThread(LargeStackThread &&other) noexcept
    : thread_(other.thread_), stop_source_(std::move(other.stop_source_)),
      valid_(other.valid_)
  {
    other.valid_ = false;
  }

  // Move assignment
  LargeStackThread &operator=(LargeStackThread &&other) noexcept
  {
    if (this != &other) {
      if (valid_) {
        request_stop();
        pthread_join(thread_, nullptr);
      }
      thread_ = other.thread_;
      stop_source_ = std::move(other.stop_source_);
      valid_ = other.valid_;
      other.valid_ = false;
    }
    return *this;
  }

  void request_stop() { stop_source_.request_stop(); }

  [[nodiscard]] std::stop_token get_stop_token() const noexcept
  {
    return stop_source_.get_token();
  }

  [[nodiscard]] bool joinable() const noexcept { return valid_; }

private:
  pthread_t thread_;
  std::stop_source stop_source_;
  bool valid_;

  template<typename Func, typename... Args>
  void start_thread(size_t stack_size, Func &&func, Args &&...args)
  {
    // Create a wrapper that captures the function and arguments
    auto *task = new std::function<void()>(
        [fn = std::forward<Func>(func), stop_token = stop_source_.get_token(),
         ... args = std::forward<Args>(args)]() mutable {
          fn(stop_token, std::forward<Args>(args)...);
        });

    pthread_attr_t attr;
    int result = pthread_attr_init(&attr);
    CHECK(result == 0);
    x23::Defer _d([&] { pthread_attr_destroy(&attr); });

    result = pthread_attr_setstacksize(&attr, stack_size);
    CHECK(result == 0);

    result = pthread_create(
        &thread_, &attr,
        [](void *arg) -> void * {
          auto *fn = static_cast<std::function<void()> *>(arg);
          (*fn)();
          delete fn;
          return nullptr;
        },
        task);

    if (result != 0) {
      panic("pthread_create failed");
    }

    valid_ = true;
  }
};

} // namespace hogd
