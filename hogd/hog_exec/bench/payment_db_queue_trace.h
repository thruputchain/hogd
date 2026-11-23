#pragma once

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <fcntl.h>
#include <format>
#include <memory>
#include <string_view>
#include <sys/mman.h>
#include <unistd.h>

#include "hogd/base/log.h"
#include "hogd/hog_exec/bench/payment_db_timing.h"
#include "x23/rpc_queue_timing.h"

namespace hogd::payment_db_queue {

struct Record {
  uint64_t block, transaction, phase, read_to_handler, handler_to_enqueue,
      enqueue_to_dequeue, dequeue_to_write_attempt;
};
static_assert(sizeof(Record) == 7 * sizeof(uint64_t));
inline constexpr size_t kBytes = (kPaymentDbSampleLimit + 1) * sizeof(Record);
inline Record *records{};
inline std::atomic<uint64_t> count{};

inline void
start(uint32_t accounts, std::string_view side)
{
  CHECK(records == nullptr);
  const char *dump_dir = std::getenv("HOG_DUMP_DIR");
  CHECK(dump_dir && *dump_dir);
  const auto path =
      std::format("{}/payment-db-queue-{}-{}.bin", dump_dir, side, accounts);
  const int fd =
      open(path.c_str(), O_CREAT | O_EXCL | O_CLOEXEC | O_RDWR, 0600);
  CHECK(fd >= 0);
  CHECK(ftruncate(fd, kBytes) == 0);
  void *mapping =
      mmap(nullptr, kBytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  close(fd);
  CHECK(mapping != MAP_FAILED);
  // The process owns the mapping through exit, including late socket writes.
  records = static_cast<Record *>(mapping);
}

inline void
report(const x23::RpcQueueTiming &sample) noexcept
{
  const auto index = count.fetch_add(1, std::memory_order_relaxed);
  if (index < kPaymentDbSampleLimit) {
    std::construct_at(
        &records[index],
        Record{sample.block, sample.transaction, sample.phase,
               sample.received ? sample.handler_begin - sample.received : 0,
               sample.handler_end ? sample.enqueued - sample.handler_end : 0,
               sample.dequeued - sample.enqueued,
               sample.write_attempt - sample.dequeued});
  }
  else if (index == kPaymentDbSampleLimit) {
    std::construct_at(&records[index], Record{0, 0, 99, 0, 0, 0, 0});
  }
}

static_assert(sizeof(x23::RpcReplyFrameTiming) == 17 * sizeof(uint64_t));
inline constexpr size_t kFrameLimit = 2048;
inline x23::RpcReplyFrameTiming *frame_records{};
inline std::atomic<uint64_t> frame_count{};

inline void
start_frames(uint32_t accounts)
{
  CHECK(frame_records == nullptr);
  const char *dump_dir = std::getenv("HOG_DUMP_DIR");
  CHECK(dump_dir && *dump_dir);
  const auto path =
      std::format("{}/payment-db-frame-client-{}.bin", dump_dir, accounts);
  const int fd =
      open(path.c_str(), O_CREAT | O_EXCL | O_CLOEXEC | O_RDWR, 0600);
  CHECK(fd >= 0);
  constexpr auto bytes = (kFrameLimit + 1) * sizeof(x23::RpcReplyFrameTiming);
  CHECK(ftruncate(fd, bytes) == 0);
  void *mapping =
      mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  close(fd);
  CHECK(mapping != MAP_FAILED);
  frame_records = static_cast<x23::RpcReplyFrameTiming *>(mapping);
}

inline void
report_frame(const x23::RpcReplyFrameTiming &sample) noexcept
{
  const auto index = frame_count.fetch_add(1, std::memory_order_relaxed);
  if (index < kFrameLimit)
    std::construct_at(&frame_records[index], sample);
  else if (index == kFrameLimit)
    std::construct_at(&frame_records[index],
                      x23::RpcReplyFrameTiming{.phase = 99});
}

} // namespace hogd::payment_db_queue
