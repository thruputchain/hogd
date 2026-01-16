#pragma once

#include <string>
#include <sys/socket.h>

#include "coord_sched.hh"
#include "db_sched.hh"
#include "event_rpc.hh"
#include "exec_sched.hh"
#include "hogd/base/coropoll.h"
#include "hogd/base/log.h"
#include "hogd/base/rpc_client.h"
#include "hogd/base/time.h"
#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h" // IWYU pragma: keep
#include "loadgen.hh"
#include "sched_coord.hh"
#include "sched_db.hh"
#include "x23/rpc.h"
#include "x23/sockio.h"

namespace hogd {

using coord_sched_client_t =
    CoordScheduler1::client<x23::RpcStream<x23::RpcLogger>::ClientRef>;

using db_sched_client_t =
    DbSched1::client<x23::RpcStream<x23::RpcLogger>::ClientRef>;

using event_sched_client_t =
    EventSched1::client<x23::RpcStream<x23::RpcLogger>::ClientRef>;

using exec_sched_client_t =
    ExecutorSched1::client<x23::RpcStream<x23::RpcLogger>::ClientRef>;

using loadgen_client_t =
    LoadgenInterfaceV1::client<x23::RpcStream<x23::RpcLogger>::ClientRef>;

using sched_coord_client_t =
    SchedulerCoord1::client<x23::RpcStream<x23::RpcLogger>::ClientRef>;

using sched_db_client_t =
    SchedDb1::client<x23::RpcStream<x23::RpcLogger>::ClientRef>;

template<typename T>
inline auto
create_client(const sockaddr_storage &ss, bool retry = false,
              size_t max_message_size = x23::default_max_message_size)
{
  return make_rpc_client<T>(ss, retry, max_message_size);
}

template<typename T>
inline auto
create_client(x23::HostPort &host_port, bool retry = false,
              size_t max_message_size = x23::default_max_message_size)
{
  auto ss_or_error = host_port.to_addr(/*dns=*/true);
  CHECK(ss_or_error.has_value());
  return create_client<T>(*ss_or_error, retry, max_message_size);
}

template<typename T>
inline auto
create_client(const std::string &host, uint16_t port, bool retry = false,
              size_t max_message_size = x23::default_max_message_size)
{
  x23::HostPort host_port{host.c_str(), port};
  return create_client<T>(host_port, retry, max_message_size);
}

// The number of coroutines to use (one coroutine per thread) when converting
// to/from a parallel batch.
constexpr size_t kNumThreadsForParallelBatch = 20;

template<typename T>
inline x23::Async<bool>
parallel_batch_to_t(const ParallelBatch &parallel, x23::XdrVec<T> &objs)
{
  ElapsedTimer timer;
  CHECK_EQ(parallel.segment_tokens.size(), parallel.segment_bytes.size());

  std::vector<size_t> byte_offsets;
  byte_offsets.reserve(parallel.segment_bytes.size());
  size_t total_segment_bytes = 0;
  for (unsigned int segment_bytes : parallel.segment_bytes) {
    byte_offsets.push_back(total_segment_bytes);
    total_segment_bytes += segment_bytes;
  }
  CHECK_EQ(total_segment_bytes, parallel.xdr.size());

  std::vector<size_t> obj_offsets;
  obj_offsets.reserve(parallel.segment_tokens.size());
  size_t total_segment_tokens = 0;
  for (unsigned int segment_tokens : parallel.segment_tokens) {
    obj_offsets.push_back(total_segment_tokens);
    total_segment_tokens += segment_tokens;
  }
  objs.resize(total_segment_tokens, {});

  const std::vector<ChunkRange> chunks =
      make_chunks(parallel.segment_tokens.size(), kNumThreadsForParallelBatch);
  x23::Vec<x23::Async<bool>> handles;
  handles.reserve(chunks.size());
  for (const ChunkRange &chunk : chunks) {
    handles.push_back([](const ChunkRange &chunk, const ParallelBatch &parallel,
                         x23::XdrVec<T> &objs,
                         const std::vector<size_t> &byte_offsets,
                         const std::vector<size_t> &obj_offsets)
                          -> x23::Async<bool> {
      co_await default_coropoll().pool_.bg();

      for (size_t i = chunk.start; i < chunk.end; i++) {
        const unsigned int num_segment_tokens = parallel.segment_tokens[i];
        const unsigned int num_segment_bytes = parallel.segment_bytes[i];

        size_t inner_obj_offset = 0;
        size_t inner_xdr_offset = 0;
        for (size_t j = 0; j < num_segment_tokens; j++) {
          try {
            size_t num_consumed = x23::deserialize_from(
                &objs[obj_offsets[i] + inner_obj_offset++],
                x23::BufView(&parallel.xdr[byte_offsets[i] + inner_xdr_offset],
                             num_segment_bytes - inner_xdr_offset));
            inner_xdr_offset += num_consumed;
          } catch (const x23::XdrBadInput &e) {
            info("failed to deserialize: {}", e.what());
            co_return false;
          }
        }
      }
      co_return true;
    }(chunk, parallel, objs, byte_offsets, obj_offsets));
  }
  bool ret = true;
  for (x23::Async<bool> &handle : handles) {
    // Careful, we deliberately have [co_await handle] as the first operand of
    // the AND statement to avoid short-circuiting. If we did it the other way
    // around ([ret && (co_await handle)]), then if any of the handles return
    // false and we set [ret] to false, we would fail to [co_await] the
    // remaining handles in [handles] due to short-circuiting.
    ret = (co_await handle) && ret;
  }
  info("parallel_batch_to_t took {}, parallel.segment_tokens.size() is {}",
       timer.elapsed(), parallel.segment_tokens.size());
  co_return ret;
}

template<typename T>
inline x23::Async<void>
t_to_parallel_batch(x23::span<T> objs, ParallelBatch &parallel)
{
  // [t] and [{t}] (i.e., a vector with [t]) serialize differently. Thus, we
  // serialize each object individually rather than serialize [objs] once.
  ElapsedTimer timer;
  // [ParallelBatch] is a struct, and the caller likely allocated it without
  // zeroing it out. Thus, we zero it out here.
  parallel = {};
  const std::vector<ChunkRange> chunks =
      make_chunks(objs.size(), kNumThreadsForParallelBatch);
  x23::Vec<x23::Async<void>> handles;
  handles.reserve(chunks.size());
  x23::Vec<x23::XdrVec<std::uint8_t>> xdrs;
  xdrs.resize(chunks.size(), {});
  parallel.segment_bytes.resize(chunks.size(), 0);
  parallel.segment_tokens.resize(chunks.size(), 0);
  for (size_t i = 0; i < chunks.size(); i++) {
    const ChunkRange &chunk = chunks[i];
    handles.push_back([](const ChunkRange &chunk, size_t i,
                         ParallelBatch &parallel,
                         x23::Vec<x23::XdrVec<std::uint8_t>> &xdrs,
                         x23::span<T> &objs) -> x23::Async<void> {
      co_await default_coropoll().pool_.bg();

      for (size_t j = chunk.start; j < chunk.end; j++) {
        xdrs[i] += x23::serialize(objs[j]);
      }
      parallel.segment_bytes[i] = xdrs[i].size();
      parallel.segment_tokens[i] = chunk.end - chunk.start;
      co_return;
    }(chunk, i, parallel, xdrs, objs));
  }
  for (size_t i = 0; i < chunks.size(); i++) {
    co_await handles[i];
    parallel.xdr += xdrs[i];
  }
  info("t_to_parallel_batch took {}", timer.elapsed());
  co_return;
}

template<typename T>
inline x23::Async<ParallelBatch>
t_to_parallel_batch(x23::span<T> objs)
{
  ParallelBatch parallel;
  co_await t_to_parallel_batch(objs, parallel);
  co_return parallel;
}

template<typename T>
inline x23::Async<void>
t_to_parallel_batch(const x23::XdrVec<T> &objs, ParallelBatch &parallel)
{
  co_await t_to_parallel_batch(x23::span<const T>(objs), parallel);
  co_return;
}

template<typename T>
inline x23::Async<ParallelBatch>
t_to_parallel_batch(const x23::XdrVec<T> &objs)
{
  ParallelBatch parallel;
  co_await t_to_parallel_batch(x23::span<const T>(objs), parallel);
  co_return parallel;
}

} // namespace hogd
