#include "hogd/hogexec/runtime/syscall_inner.h"

#include <cstddef>
#include <span>
#include <utility>

#include "hogd/base/types.h"
#include "hogd/crypto/crypto.h"
#include "hogd/hogexec/runtime/gas_cost_table.h"
#include "hogd/hogexec/runtime/status.h"
#include "syscall.hh"
#include "types.hh"

namespace hogd {

std::pair<HostFnStatus<SyscallErrorCode>, GasAmount>
syscall_SHA256_inner(std::span<const std::byte> input_buffer,
                     SHA256Hash &output_hash)
{
  output_hash = sha256(input_buffer);
  return {SyscallErrorCode::kNone, SHA256HashCost(input_buffer.size())};
};

} // namespace hogd
