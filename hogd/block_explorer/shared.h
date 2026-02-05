#include "indexer.hh"
#include "x23/rpc.h"

namespace hogd {

using indexer_block_explorer_client_t =
    IndexerBlockExplorer1::client<x23::RpcStream<x23::RpcLogger>::ClientRef>;

} // namespace hogd
