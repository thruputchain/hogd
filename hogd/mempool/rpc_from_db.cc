#include "hogd/mempool/rpc_from_db.h"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstddef>
#include <future>
#include <span>
#include <tuple>
#include <vector>

#include "db_sched.hh"
#include "discovery.hh"
#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "hogd/base/worker_pool.h"
#include "hogd/container/merkle/hex_array.h"
#include "hogd/container/merkle/merkle_trie.h"
#include "hogd/mempool/authentication.h"
#include "nomination.hh"
#include "rpc_types.hh"
#include "sched_db.hh"
#include "transaction.hh"
#include "types.hh"
#include "x23/async.h"
#include "x23/vec.h"

namespace hogd {

x23::Async<void>
MempoolDbRpc::scheddb_intro_shard(ShardCard card)
{
  co_return;
}

} // namespace hogd
