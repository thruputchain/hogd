#include "hogd/test_ffi/syscall_handler_wrapper.h"

#include <memory>
#include <utility>

#include "ffi_enums.hh"
#include "hogd/test_ffi/db_context_wrapper.h"
#include "hogd/test_ffi/mock_runtime.h"
#include "hogd/test_ffi/test_bindings.h"
#include "transaction.hh"
#include "x23/cleanup.h"

namespace hogd {

SyscallHandlerContext::SyscallHandlerContext(
    std::shared_ptr<MockRuntimeContext> context, SignedTransaction const &stx,
    DBContext &db)
  : handler_(stx, db.get_snapshot(), std::move(context))
{}

ForeignRootInvokeResult
SyscallHandlerContext::root_invoke()
{
  if (!push_context()) {
    return ForeignRootInvokeResult{
        .result = ForeignInvokeResult{.result_code = 9,
                                      .panic = std::to_underlying(
                                          InvokeErrorCode::kUnrecoverable)},
        .gas_consumed = 0};
  }
  x23::Defer _([this] () {
    pop_context();
  });

  auto res = handler_.root_invoke();

  if (res.result_) {
    return ForeignRootInvokeResult{
        .result = ForeignInvokeResult{.result_code = *res.result_,
                                      .panic = std::to_underlying(
                                          InvokeErrorCode::kNone)},
        .gas_consumed = res.gas_consumed_};
  }

  return ForeignRootInvokeResult{.result = {.result_code = 0,
                                            .panic = std::to_underlying(
                                                res.result_.error().code())},
                                 .gas_consumed = res.gas_consumed_};
}

} // namespace hogd
