#include "hogd/mempool/rpc_from_exec.h"

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
#include "exec_sched.hh"
#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "hogd/base/worker_pool.h"
#include "hogd/container/merkle/hex_array.h"
#include "hogd/container/merkle/merkle_trie.h"
#include "hogd/mempool/authentication.h"
#include "nomination.hh"
#include "rpc_types.hh"
#include "sched_exec.hh"
#include "transaction.hh"
#include "types.hh"
#include "x23/async.h"
#include "x23/vec.h"

namespace hogd {

x23::Async<IntroExecResult>
MempoolExecutorRpc::schedexec_intro_exec(ExecCard card)
{
  // TODO(jhumphri): Fill this in.
  co_return IntroExecResult{.exec_id = 0, .shards = {}};
}

} // namespace hogd
