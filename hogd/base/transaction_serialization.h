#pragma once

#include <cstddef>
#include <span>
#include <vector>

#include "hogd/base/coropoll.h"
#include "hogd/base/os.h"
#include "transaction.hh"
#include "x23/async.h"
#include "x23/serialize.h"
#include "x23/vec.h"

namespace hogd {

using SerializedTransactionBatch = x23::Vec<x23::Buf>;

inline void
serialize_transaction_payload_to(const SignedTransaction &tx, x23::Buf &out)
{
  out.clear();
  x23::serialize_to(&out, tx.tx);
}

inline x23::Async<void>
serialize_transaction_payloads(std::span<const SignedTransaction> txs,
                               SerializedTransactionBatch &serialized_txs,
                               size_t max_parallelism = available_parallelism())
{
  serialized_txs.clear();
  serialized_txs.resize(txs.size());

  const std::vector<ChunkRange> chunks = make_chunks(txs.size(), max_parallelism);
  x23::Vec<x23::Async<void>> handles;
  handles.reserve(chunks.size());
  for (const ChunkRange &chunk : chunks) {
    handles.push_back(
        [](ChunkRange chunk, std::span<const SignedTransaction> txs,
           SerializedTransactionBatch &serialized_txs) noexcept
        -> x23::Async<void> {
          co_await default_coropoll().pool_.bg();
          for (size_t i = chunk.start; i < chunk.end; i++) {
            serialize_transaction_payload_to(txs[i], serialized_txs[i]);
          }
          co_return;
        }(chunk, txs, serialized_txs));
  }
  for (x23::Async<void> &handle : handles) {
    co_await handle;
  }
  co_return;
}

inline x23::Async<SerializedTransactionBatch>
serialize_transaction_payloads(std::span<const SignedTransaction> txs,
                               size_t max_parallelism = available_parallelism())
{
  SerializedTransactionBatch serialized_txs;
  co_await serialize_transaction_payloads(txs, serialized_txs, max_parallelism);
  co_return serialized_txs;
}

} // namespace hogd
