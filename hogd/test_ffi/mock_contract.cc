#include "hogd/test_ffi/mock_contract.h"

#include "ffi_enums.hh"
#include "hogd/base/log.h"
#include "hogd/hogexec/common/status.h"
#include "hogd/hogexec/runtime/runtime.h"
#include <string>
#include <string_view>

namespace hogd {

InvokeStatus<void>
MockContract::invoke(std::string_view method)
{
  auto id = Runtime::method_id_from_name(method);
  if (!id.has_value()) {
    error("Unrecoverable: MockContract::invoke failed to parse method name "
          "'{}'",
          method);
    return InvokeError{InvokeErrorCode::kInvokeUnrecoverable};
  }
  auto res = entry_point_(foreign_context_, *id);

  auto code = static_cast<InvokeErrorCode>(res.panic);

  if (code == InvokeErrorCode::kInvokeNone) {
    return {};
  }
  return InvokeError{code};
}
} // namespace hogd
