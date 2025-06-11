// merkle.h - provides a merkle patricia trie

#pragma once

#include <algorithm>
#include <bit>
#include <cassert>
#include <functional>
#include <initializer_list>
#include <iterator>
#include <memory>
#include <mutex>
#include <ranges>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <vector>

#include "hogd/base/compiler.h"
#include "hogd/base/os.h"
#include "hogd/base/types.h"
#include "hogd/base/worker_pool.h"

// TODO: Support template types. For now, the keys are strings and the values
// are [int64_t]s.
namespace hogd {

// TODO: Describe the class.
//
// Example:
// TODO: Provide example.
class MerkleTrie {
public:
  // type definitions
  using Key = std::string;
  using T = int64_t;

  MerkleTrie() {}
  ~MerkleTrie() = default;

  // disable copy and support move.
  MerkleTrie(const MerkleTrie &) = delete;
  MerkleTrie &operator=(const MerkleTrie &) = delete;
  MerkleTrie(MerkleTrie &&) noexcept = default;
  MerkleTrie &operator=(MerkleTrie &&) noexcept = default;

  // TODO: For all of the methods below, should they include a mutex as well (in
  // a pair)?

  std::string &operator[](const Key &key)
  {
    static std::string kEmpty = "";

    assert(false);
    return kEmpty;
  }

  std::string *find(const Key &key)
  {
    static std::string kEmpty = "";

    assert(false);
    return &kEmpty;
  }

  std::string *insert(const Key &k, T &&obj)
  {
    static std::string kEmpty = "";

    assert(false);
    return &kEmpty;
  }

  std::string *insert(const Key &k, const T &obj)
  {
    static std::string kEmpty = "";

    assert(false);
    return &kEmpty;
  }

  std::string &insert_or_assign(const Key &k, T &&obj)
  {
    static std::string kEmpty = "";

    assert(false);
    return kEmpty;
  }

  size_t remove(const Key &key)
  {
    assert(false);
    return 0;
  }

  // TODO: Add another [remove] function that also takes a mutex as a parameter.

  size_t size()
  {
    assert(false);
    return 0;
  }

  void clear() { assert(false); }

  // for_each executes func on each element.
  template<typename F> void for_each(F func) { assert(false); }

  // for_each_parallel executes func in parallel on each element using a
  // WorkerPool.
  template<typename F>
  void for_each_parallel(F func, size_t width = kDefaultParallelRangeWidth,
                         WorkerPool &pool = WorkerPool::get_default_pool())
  {
    assert(false);
  }

private:
};

} // namespace hogd
