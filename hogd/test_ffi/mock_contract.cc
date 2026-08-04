#include "hogd/test_ffi/mock_contract.h"

#include "ffi_enums.hh"
#include "hogd/hog_exec/common/status.h"
#include "hogd/hog_exec/runtime/runtime.h"

namespace hogd {

InvokeStatus<void>
MockContract::invoke(Runtime::MethodID method_id)
{
  auto res = entry_point_(foreign_context_, method_id);

  auto code = static_cast<InvokeErrorCode>(res.panic);

  if (code == InvokeErrorCode::None) {
    return {};
  }
  return InvokeError{code};
}
} // namespace hogd
