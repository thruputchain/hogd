#include "hogd/shared_util/connect_to_service.h"

#include <optional>
#include <string>

#include "coord.hh"
#include "x23/event.h"
#include "x23/rpc.h"

namespace hogd {

std::optional<hogd::CoordinatorAllocateV1::client<
    x23::RpcStream<x23::RpcLogger>::ClientRef>>
connect_to_coord_allocate_service(x23::CoroPoll &cp,
                                  std::string const &coord_address,
                                  std::string const &coord_allocate_port)
{
  auto fd = cp.tcpconnect(coord_address, coord_allocate_port)();
  if (!fd) {
    return std::nullopt;
  }

  auto client_config = x23::rpc_stream_logged(cp, *fd);
  return std::make_optional(
      client_config.template client<CoordinatorAllocateV1>());
}

} // namespace hogd
