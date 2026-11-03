#pragma once

#include "x23/vec.h"

namespace x23 {
class CpuMap;
} // namespace x23

namespace hogd {

// How many connections the executor opens to hog_db, derived from the machine.
//
// Half a connection per physical core, floored at 1 and capped at the CPU
// count by db_connection_map().  See that function for why the count is sized
// this way and what it was measured against.
int db_connection_count();

// The CpuMap the executor's hog_db multiclient is built from.
//
// One reader coroutine runs per connection, and that reader is the whole
// return path for its connection: it parses every reply header and resolves
// every xid before anything downstream can run.  So the connection count sets
// how much of the return path can proceed at once, and it is a throughput
// knob rather than a cache-topology question -- which is why this is its own
// map instead of x23::llc_map or x23::core_map.
//
// The reference is to a function-local static and is stable for the process:
// PerCpuSet holds a reference to the map rather than a copy, so it must
// outlive every client built from it.
const x23::CpuMap &db_connection_map();

// The cpu -> connection vector behind db_connection_map(), for nbuckets
// connections over the allowed CPU ids (ascending).  No connection serves
// CPUs of two LLCs unless nbuckets is below the LLC count.  Exposed for tests.
x23::Vec<int> db_connection_buckets(const x23::Vec<int> &allowed, int nbuckets,
                                    const x23::CpuMap &llc,
                                    const x23::CpuMap &core);

} // namespace hogd
