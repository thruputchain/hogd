#pragma once

#include <cstdint>

namespace hogd {

// The id a runtime dispatches an invocation on: a ContractEntrypointID read as
// a big-endian uint64_t. Runtime::MethodID is this type; it lives here so that
// entrypoint derivation (contract_trait.h) does not depend on the runtime.
using MethodID = uint64_t;

} // namespace hogd
