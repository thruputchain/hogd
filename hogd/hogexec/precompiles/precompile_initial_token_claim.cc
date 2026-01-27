#include "hogd/hogexec/precompiles/precompile_initial_token_claim.h"

#include <algorithm>
#include <cassert>
#include <limits>
#include <memory>
#include <span>
#include <type_traits>
#include <utility>

#include "built_contracts/initial_token_claim.h"
#include "ffi_enums.hh"
#include "hogd/base/compiler.h"
#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h"
#include "hogd/crypto/crypto.h"
#include "hogd/event/utilities.h"
#include "hogd/hogexec/precompiles/native_contract_builder.h"
#include "hogd/hogexec/runtime/contract_result.h"
#include "hogd/hogexec/runtime/native_runtime.h"
#include "hogd/hogexec/runtime/serialize_calldata.h"
#include "hogd/hogexec/runtime/status.h"
#include "hogd/hogexec/runtime/syscall_handler.h"
#include "standard_asset_events.hh"
#include "storage_item.hh"
#include "types.hh"
#include "x23/format.h"

namespace hogd {



std::unique_ptr<NativeContract> make_native_initial_token_claim_contract()
{
    return nullptr;
}
} // namespace hogd
