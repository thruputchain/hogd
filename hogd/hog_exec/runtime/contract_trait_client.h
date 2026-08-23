#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <utility>

#include "hogd/base/cpputil.h"
#include "hogd/base/log.h"
#include "hogd/base/string.h"
#include "hogd/hog_exec/common/status.h"
#include "hogd/hog_exec/runtime/contract_trait.h"
#include "hogd/hog_exec/runtime/native_syscall_context.h"
#include "hogd/hog_exec/runtime/runtime.h"
#include "hogd/hog_exec/runtime/syscall_handler.h"
#include "invoke_flag.hh"
#include "syscall.hh"
#include "syscall_error.hh"
#include "transaction.hh"
#include "types.hh"
#include "x23/format.h"
#include "x23/async.h"

namespace hogd {

template<is_contract_trait ContractTrait> class ContractTraitClient {
public:
  ContractTraitClient(ContractAddress contract_address)
    : contract_address_(contract_address)
  {}

  using trait_meta_t = ContractTraitMeta<ContractTrait>;

  template<StringLiteral Name, typename... Args>
  Invocation make_invocation(GasAmount gas_amount, Args &&...args)
  {
    auto calldata =
        trait_meta_t::template make_calldata<Name>(std::forward<Args>(args)...);
    return Invocation{.invoked_address = contract_address_,
                      .entrypoint = trait_meta_t::template entrypoint<Name>(),
                      .gas_limit = gas_amount,
                      .calldata = calldata};
  }

  template<StringLiteral Name,
           InvokeFlagSet Flags = std::to_underlying(InvokeFlag::None)>
  auto invoke_opt(NativeSyscallContext &ctx, auto &&...args)
      -> x23::Async<HostFnStatus<SyscallStatus<
          std::optional<typename trait_meta_t::template return_t<Name>>>>>
  {
    auto calldata = trait_meta_t::template make_calldata<Name>(
        std::forward<decltype(args)>(args)...);
    trace("Invoke method {} (Name {}) calldata: {}",
          encode_hex(std::as_bytes(
              std::span(trait_meta_t::template entrypoint<Name>().data(), 8))),
          std::string_view(reinterpret_cast<const char *>(Name.get().data()),
                           Name.get().size()),
          x23::show(calldata));

    CopyableInvokeOutputs outputs{};
    CopyableInvokeInputs inputs{
        .entrypoint_id =
            sdk_method_id(trait_meta_t::template entrypoint<Name>()),
        .gas_limit = static_cast<GasAmount>(UINT64_MAX),
        .flags = Flags,
    };

    auto invoke_result = co_await ctx.template syscall<SyscallNo::kInvoke>(
        contract_address_,
        std::as_bytes(std::span(calldata.data(), calldata.size())),
        std::span<const std::byte>{}, inputs, outputs);

    if (!invoke_result.has_value()) {
      trace("Invoke HostFnError: {}", invoke_result.error());
      co_return invoke_result.error();
    }

    // A nonzero error code means the invoke was rejected before a child frame
    // ran. When a frame ran (code == None), the outcome (Return / Abort /
    // Fault) is reported via outputs.frame_status; a Fault carries no return
    // buffer and is surfaced below via get_return_buffer().
    if (*invoke_result != SyscallErrorCode::kSyscallErrorCodeNone) {
      trace("Invoke SyscallStatus error: {}", *invoke_result);
      co_return SyscallError{*invoke_result};
    }

    auto return_buffer = ctx.handler().get_return_buffer();
    if (!return_buffer.has_value()) {
      co_return return_buffer.error().to_host_fn_error();
    }
    assert(return_buffer);
    // A Fault frame_status leaves no return buffer available (the syscall
    // succeeded but the child frame produced no buffer), so this must be
    // handled, not asserted: surface it as the syscall error.
    if (!(*return_buffer).has_value()) {
      trace("No return buffer available: {}", (*return_buffer).error());
      co_return (*return_buffer).error();
    }
    assert(*return_buffer);
    assert(**return_buffer);
    auto const *unwrapped_return_buffer = **return_buffer;
    assert(unwrapped_return_buffer);
    if (outputs.return_buffer_len != unwrapped_return_buffer->size()) {
      trace("Unrecoverable: Return buffer size mismatch: {} != {}",
            outputs.return_buffer_len, unwrapped_return_buffer->size());
      co_return HostFnError{HostFnErrorCode::Unrecoverable};
    }
    std::span<const std::byte> return_span =
        std::as_bytes(std::span(unwrapped_return_buffer->data(),
                                unwrapped_return_buffer->size()));

    trace("Received return buffer: {}", x23::show(*unwrapped_return_buffer));

    bool manually_reverted =
        outputs.frame_status == InvocationFrameStatusCode::Abort;

    auto return_value =
        trait_meta_t::template decode_return<Name>(return_span,
                                                   manually_reverted);
    if (!return_value.has_value()) {
      trace("Invoke Error: Decode Return Failed");
      co_return std::nullopt;
    }
    co_return *return_value;
  }

private:
  const ContractAddress contract_address_;
};

} // namespace hogd
