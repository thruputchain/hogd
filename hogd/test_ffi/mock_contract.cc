#include "hogd/test_ffi/mock_contract.h"

#include "ffi_enums.hh"
#include "hogd/base/types.h"
#include "hogd/hogexec/runtime/runtime.h"
#include "hogd/hogexec/runtime/status.h"
#include <string>
#include <string_view>

namespace hogd {

InvokeStatus<int32_t>
MockContract::invoke(std::string_view method)
{
  auto id = Runtime::method_id_from_name(std::string(method));
  if (!id.has_value()) {
    return InvokeError{InvokeErrorCode::kInvokeUnrecoverable};
  }
  auto res = entry_point_(foreign_context_, *id);

  auto code = static_cast<InvokeErrorCode>(res.panic);

  if (code == InvokeErrorCode::kInvokeNone) {
    return InvokeStatus<int32_t>{res.result_code};
  }
  return InvokeError{code};
}
} // namespace hogd
