#include "indexer.hh"
#include "x23/rpc.h"

namespace hogd {

using event_indexer_client_t =
    EventIndexer1::client<x23::RpcStream<x23::RpcLogger>::ClientRef>;

} // namespace hogd
