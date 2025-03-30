#include "hogd/test_ffi/db_context_wrapper.h"

#include <cstdlib>
#include <memory>

#include "hogd/hogexec/runtime/storage_snapshot_cache.h"

namespace hogd {

DBContext::DBContext() : snapshot_cache_(nullptr)
{}

void
DBContext::advance_new_block()
{
  snapshot_cache_ = std::make_unique<StorageSnapshotCache>(5);
}

} // namespace hogd
