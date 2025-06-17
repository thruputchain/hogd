// -*-C++-*-

#pragma once

#include <x23/rpc_common.h>
#include <x23/vec.h>

namespace x23 {

struct SRpcService {
  virtual ~SRpcService() {}
  virtual std::uint32_t program() const = 0;
  virtual std::uint32_t version() const = 0;
  virtual Buf serve(const rpc_msg &msg, BufView args) = 0;
  ProgVers getpv() const
  {
    return ProgVers{.prog = program(), .vers = version()};
  }
};

} // namespace x23
