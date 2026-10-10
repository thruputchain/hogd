#!/usr/bin/env python3
from pathlib import Path

path = Path("hogd/container/concurrent_map.h")
s = path.read_text()
if "diagnostic_buckets" not in s:
    s = s.replace("#include <algorithm>", "#include <algorithm>\n#include <cstdlib>")
    s = s.replace("  ConcurrentMap(size_t", """  const void *diagnostic_buckets() const { return buckets_.data(); }
  const void *diagnostic_allocator() const { return value_allocator_.get(); }

  ConcurrentMap(size_t""", 1)
    s = s.replace("  static Storage acquire_storage(size_t n_buckets)\n  {", """  static Storage acquire_storage(size_t n_buckets)
  {
    if (std::getenv("HOG_LOCK_NO_RECYCLE") != nullptr) {
      return Storage{bucket_array_t(n_buckets), std::make_unique<value_allocator_t>()};
    }""")
    s = s.replace("  static void release_storage(Storage storage) noexcept\n  {", """  static void release_storage(Storage storage) noexcept
  {
    if (std::getenv("HOG_LOCK_NO_RECYCLE") != nullptr) {
      return;
    }""")
    path.write_text(s)

path = Path("hogd/hog_db/block_tx.h")
s = path.read_text()
assert "LOCKSTORAGE" not in s
s = s.replace("#include <atomic>", "#include <atomic>\n#include <cstdio>")
old = "  {}\n  ~BlockTX() = default;"
assert old in s
s = s.replace(old, """  {
    std::fprintf(stderr,
        "LOCKSTORAGE height=%lld block=%p addr_buckets=%p item_buckets=%p addr_allocator=%p item_allocator=%p\\n",
        static_cast<long long>(block_height_), static_cast<void *>(this),
        addr_balance_locks_.diagnostic_buckets(), item_locks_.diagnostic_buckets(),
        addr_balance_locks_.diagnostic_allocator(), item_locks_.diagnostic_allocator());
  }
  ~BlockTX() = default;""")
path.write_text(s)
