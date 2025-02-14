#pragma once

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
                                  std::string const &coord_allocate_port);

} // namespace hogd
