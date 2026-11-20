#pragma once

#include <cstdint>
#include <type_traits>

#include "auth_rpc.hh"
#include "hogd/base/chunk_items.h"
#include "hogd/base/types.h"
#include "types.hh"
#include "x23/serialize.h"
#include "x23/x23.h"

namespace hogd {

// A KeyChangeChunk whose changes are still in the parts they were produced
// in. It serializes identically to the wire struct, so a client sends it as
// one without copying a change.
struct KeyChangeChunkView {
  BlockHeight height;
  std::uint32_t chunk_index;
  std::uint32_t chunk_count;
  ChunkItems<KeyChange> changes;
};

inline size_t
xdr_size(const KeyChangeChunkView &view)
{
  return x23::xdr_size(view.height) + x23::xdr_size(view.chunk_index) +
         x23::xdr_size(view.chunk_count) + xdr_size(view.changes);
}

void
xdr_serialize(auto &serializer, const KeyChangeChunkView &view)
{
  serializer(view.height);
  serializer(view.chunk_index);
  serializer(view.chunk_count);
  serializer(view.changes);
}

} // namespace hogd

template<>
struct x23::serialize_equiv<hogd::KeyChangeChunk, hogd::KeyChangeChunkView>
  : std::true_type {};
