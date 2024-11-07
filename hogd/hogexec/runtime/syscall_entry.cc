#include "hogd/hogexec/runtime/syscall_entry.h"

#include <array>
#include <cassert>
#include <cstddef>
#include <cstring>
#include <optional>
#include <span>
#include <type_traits>
#include <utility>

#include "hogd/base/compiler.h"
#include "hogd/base/types.h"
#include "hogd/hogexec/runtime/gas_cost_table.h"
#include "hogd/hogexec/runtime/status.h"
#include "hogd/hogexec/runtime/syscall_argument_parsers.h"
#include "hogd/hogexec/runtime/syscall_inner.h"
#include "hogd/hogexec/runtime/wasmi_runtime.h"
#include "syscall.hh"
#include "types.hh"

namespace hogd {

template<typename T>
std::pair<HostFnErrorCode, int32_t>
convert_return_t(T const &value)
{
  if constexpr (std::is_same_v<T, HostFnStatus<SyscallErrorCode>>) {
    if (value) {
      return {HostFnErrorCode::kNoneOrRecoverable,
              static_cast<int32_t>(*value)};
    }
    else {
      return {value.error().code(), 0};
    }
  }
  else {
    static_assert(std::is_same_v<T, std::pair<HostFnErrorCode, int32_t>>);
    return value;
  }
}

std::pair<HostFnResult, GasAmount>
syscall_SHA256_outer(SystemCallHandler &syscall_handler,
                     std::span<std::byte> contract_memory,
                     uint64_t user_input_buffer, uint64_t user_input_buffer_len,
                     uint64_t user_output_hash, uint64_t /*unused*/,
                     uint64_t /*unused*/, uint64_t /*unused*/)
{
  std::optional<std::span<const std::byte>> input_buffer =
      check_user_span_argument(contract_memory, user_input_buffer,
                               user_input_buffer_len);

  if (!input_buffer) {
    return {{HostFnErrorCode::kNoneOrRecoverable,
             static_cast<int32_t>(SyscallErrorCode::kFault)},
            kGenericBadSyscallCost};
  }

  SHA256Hash *output_hash =
      check_user_argument<SHA256Hash>(contract_memory, user_output_hash,
                                      sizeof(SHA256Hash));

  if (!output_hash) {
    return {{HostFnErrorCode::kNoneOrRecoverable,
             static_cast<int32_t>(SyscallErrorCode::kFault)},
            kGenericBadSyscallCost};
  }

  auto res = syscall_SHA256_inner(*input_buffer, *output_hash);

  return {convert_return_t(res.first), res.second};
}
std::pair<HostFnResult, GasAmount>
syscall_GetMsgSender_outer(SystemCallHandler &syscall_handler,
                           std::span<std::byte> contract_memory,
                           uint64_t user_output_sender, uint64_t /*unused*/,
                           uint64_t /*unused*/, uint64_t /*unused*/,
                           uint64_t /*unused*/, uint64_t /*unused*/)
{
  ContractAddress *output_sender =
      check_user_argument<ContractAddress>(contract_memory, user_output_sender,
                                           sizeof(ContractAddress));

  if (!output_sender) {
    return {{HostFnErrorCode::kNoneOrRecoverable,
             static_cast<int32_t>(SyscallErrorCode::kFault)},
            kGenericBadSyscallCost};
  }

  auto res = syscall_GetMsgSender_inner(syscall_handler, *output_sender);

  return {convert_return_t(res.first), res.second};
}

std::pair<HostFnResult, GasAmount>
syscall_GetCalldataLen_outer(SystemCallHandler &syscall_handler,
                             std::span<std::byte> contract_memory,
                             uint64_t user_output_len, uint64_t /*unused*/,
                             uint64_t /*unused*/, uint64_t /*unused*/,
                             uint64_t /*unused*/, uint64_t /*unused*/)
{
  RuntimeMemoryLength *output_len =
      check_user_argument<RuntimeMemoryLength>(contract_memory, user_output_len,
                                               sizeof(RuntimeMemoryLength));

  if (!output_len) {
    return {{HostFnErrorCode::kNoneOrRecoverable,
             static_cast<int32_t>(SyscallErrorCode::kFault)},
            kGenericBadSyscallCost};
  }

  auto res = syscall_GetCalldataLen_inner(syscall_handler, *output_len);

  return {convert_return_t(res.first), res.second};
}
std::pair<HostFnResult, GasAmount>
syscall_GetCalldata_outer(SystemCallHandler &syscall_handler,
                          std::span<std::byte> contract_memory,
                          uint64_t user_output_buffer,
                          uint64_t user_output_buffer_len,
                          uint64_t user_output_written_len, uint64_t /*unused*/,
                          uint64_t /*unused*/, uint64_t /*unused*/)
{
  std::optional<std::span<std::byte>> output_buffer =
      check_user_span_argument(contract_memory, user_output_buffer,
                               user_output_buffer_len);

  if (!output_buffer) {
    return {{HostFnErrorCode::kNoneOrRecoverable,
             static_cast<int32_t>(SyscallErrorCode::kFault)},
            kGenericBadSyscallCost};
  }

  RuntimeMemoryLength *output_written_len =
      check_user_argument<RuntimeMemoryLength>(contract_memory,
                                               user_output_written_len,
                                               sizeof(RuntimeMemoryLength));

  if (!output_written_len) {
    return {{HostFnErrorCode::kNoneOrRecoverable,
             static_cast<int32_t>(SyscallErrorCode::kFault)},
            kGenericBadSyscallCost};
  }

  auto res = syscall_GetCalldata_inner(syscall_handler, *output_buffer,
                                       *output_written_len);

  return {convert_return_t(res.first), res.second};
}
std::pair<HostFnResult, GasAmount>
syscall_Log_outer(SystemCallHandler &syscall_handler,
                  std::span<std::byte> contract_memory,
                  uint64_t user_input_buffer, uint64_t user_input_buffer_len,
                  uint64_t /*unused*/, uint64_t /*unused*/, uint64_t /*unused*/,
                  uint64_t /*unused*/)
{
  std::optional<std::span<const std::byte>> input_buffer =
      check_user_span_argument(contract_memory, user_input_buffer,
                               user_input_buffer_len);

  if (!input_buffer) {
    return {{HostFnErrorCode::kNoneOrRecoverable,
             static_cast<int32_t>(SyscallErrorCode::kFault)},
            kGenericBadSyscallCost};
  }

  auto res = syscall_Log_inner(syscall_handler, *input_buffer);

  return {convert_return_t(res.first), res.second};
}
std::pair<HostFnResult, GasAmount>
syscall_Invoke_outer(SystemCallHandler &syscall_handler,
                     std::span<std::byte> contract_memory,
                     uint64_t user_input_address, uint64_t user_input_method,
                     uint64_t user_input_gas_limit,
                     uint64_t user_input_calldata,
                     uint64_t user_input_calldata_len,
                     uint64_t user_output_return_len)
{
  const ContractAddress *input_address =
      check_user_argument<const ContractAddress>(contract_memory,
                                                 user_input_address,
                                                 sizeof(ContractAddress));

  if (!input_address) {
    return {{HostFnErrorCode::kNoneOrRecoverable,
             static_cast<int32_t>(SyscallErrorCode::kFault)},
            kGenericBadSyscallCost};
  }

  const MethodID input_method =
      check_user_primitive_argument<const MethodID>(user_input_method);

  const GasAmount input_gas_limit =
      check_user_primitive_argument<const GasAmount>(user_input_gas_limit);

  std::optional<std::span<const std::byte>> input_calldata =
      check_user_span_argument(contract_memory, user_input_calldata,
                               user_input_calldata_len);

  if (!input_calldata) {
    return {{HostFnErrorCode::kNoneOrRecoverable,
             static_cast<int32_t>(SyscallErrorCode::kFault)},
            kGenericBadSyscallCost};
  }

  RuntimeMemoryLength *output_return_len =
      check_user_argument<RuntimeMemoryLength>(contract_memory,
                                               user_output_return_len,
                                               sizeof(RuntimeMemoryLength));

  if (!output_return_len) {
    return {{HostFnErrorCode::kNoneOrRecoverable,
             static_cast<int32_t>(SyscallErrorCode::kFault)},
            kGenericBadSyscallCost};
  }

  auto res = syscall_Invoke_inner(syscall_handler, *input_address, input_method,
                                  input_gas_limit, *input_calldata,
                                  *output_return_len);

  return {convert_return_t(res.first), res.second};
}

std::pair<HostFnResult, GasAmount>
syscall_Return_outer(SystemCallHandler &syscall_handler,
                     std::span<std::byte> contract_memory,
                     uint64_t user_input_buffer, uint64_t user_input_buffer_len,
                     uint64_t /*unused*/, uint64_t /*unused*/,
                     uint64_t /*unused*/, uint64_t /*unused*/)
{
  std::optional<std::span<const std::byte>> input_buffer =
      check_user_span_argument(contract_memory, user_input_buffer,
                               user_input_buffer_len);

  if (!input_buffer) {
    return {{HostFnErrorCode::kNoneOrRecoverable,
             static_cast<int32_t>(SyscallErrorCode::kFault)},
            kGenericBadSyscallCost};
  }

  auto res = syscall_Return_inner(syscall_handler, *input_buffer);

  return {convert_return_t(res.first), res.second};
}
std::pair<HostFnResult, GasAmount>
syscall_GetReturnedBuffer_outer(SystemCallHandler &syscall_handler,
                                std::span<std::byte> contract_memory,
                                uint64_t user_output_buffer,
                                uint64_t user_output_buffer_len,
                                uint64_t user_output_written_len,
                                uint64_t /*unused*/, uint64_t /*unused*/,
                                uint64_t /*unused*/)
{
  std::optional<std::span<std::byte>> output_buffer =
      check_user_span_argument(contract_memory, user_output_buffer,
                               user_output_buffer_len);

  if (!output_buffer) {
    return {{HostFnErrorCode::kNoneOrRecoverable,
             static_cast<int32_t>(SyscallErrorCode::kFault)},
            kGenericBadSyscallCost};
  }

  RuntimeMemoryLength *output_written_len =
      check_user_argument<RuntimeMemoryLength>(contract_memory,
                                               user_output_written_len,
                                               sizeof(RuntimeMemoryLength));

  if (!output_written_len) {
    return {{HostFnErrorCode::kNoneOrRecoverable,
             static_cast<int32_t>(SyscallErrorCode::kFault)},
            kGenericBadSyscallCost};
  }

  auto res = syscall_GetReturnedBuffer_inner(syscall_handler, *output_buffer,
                                             *output_written_len);

  return {convert_return_t(res.first), res.second};
}
std::pair<HostFnResult, GasAmount>
syscall_GetDeployAddress_outer(SystemCallHandler &syscall_handler,
                               std::span<std::byte> contract_memory,
                               uint64_t user_input_nonce,
                               uint64_t user_input_contract_hash,
                               uint64_t user_output_to_be_deployed,
                               uint64_t /*unused*/, uint64_t /*unused*/,
                               uint64_t /*unused*/)
{
  const SHA256Hash *input_nonce =
      check_user_argument<const SHA256Hash>(contract_memory, user_input_nonce,
                                            sizeof(SHA256Hash));

  if (!input_nonce) {
    return {{HostFnErrorCode::kNoneOrRecoverable,
             static_cast<int32_t>(SyscallErrorCode::kFault)},
            kGenericBadSyscallCost};
  }

  const SHA256Hash *input_contract_hash =
      check_user_argument<const SHA256Hash>(contract_memory,
                                            user_input_contract_hash,
                                            sizeof(SHA256Hash));

  if (!input_contract_hash) {
    return {{HostFnErrorCode::kNoneOrRecoverable,
             static_cast<int32_t>(SyscallErrorCode::kFault)},
            kGenericBadSyscallCost};
  }

  ContractAddress *output_to_be_deployed =
      check_user_argument<ContractAddress>(contract_memory,
                                           user_output_to_be_deployed,
                                           sizeof(ContractAddress));

  if (!output_to_be_deployed) {
    return {{HostFnErrorCode::kNoneOrRecoverable,
             static_cast<int32_t>(SyscallErrorCode::kFault)},
            kGenericBadSyscallCost};
  }

  auto res = syscall_GetDeployAddress_inner(syscall_handler, *input_nonce,
                                            *input_contract_hash,
                                            *output_to_be_deployed);

  return {convert_return_t(res.first), res.second};
}
std::pair<HostFnResult, GasAmount>
syscall_DeployContract_outer(SystemCallHandler &syscall_handler,
                             std::span<std::byte> contract_memory,
                             uint64_t user_input_nonce,
                             uint64_t user_input_contract_hash,
                             uint64_t user_output_deployed_address,
                             uint64_t /*unused*/, uint64_t /*unused*/,
                             uint64_t /*unused*/)
{
  const SHA256Hash *input_nonce =
      check_user_argument<const SHA256Hash>(contract_memory, user_input_nonce,
                                            sizeof(SHA256Hash));

  if (!input_nonce) {
    return {{HostFnErrorCode::kNoneOrRecoverable,
             static_cast<int32_t>(SyscallErrorCode::kFault)},
            kGenericBadSyscallCost};
  }

  const SHA256Hash *input_contract_hash =
      check_user_argument<const SHA256Hash>(contract_memory,
                                            user_input_contract_hash,
                                            sizeof(SHA256Hash));

  if (!input_contract_hash) {
    return {{HostFnErrorCode::kNoneOrRecoverable,
             static_cast<int32_t>(SyscallErrorCode::kFault)},
            kGenericBadSyscallCost};
  }

  ContractAddress *output_deployed_address =
      check_user_argument<ContractAddress>(contract_memory,
                                           user_output_deployed_address,
                                           sizeof(ContractAddress));

  if (!output_deployed_address) {
    return {{HostFnErrorCode::kNoneOrRecoverable,
             static_cast<int32_t>(SyscallErrorCode::kFault)},
            kGenericBadSyscallCost};
  }

  auto res = syscall_DeployContract_inner(syscall_handler, *input_nonce,
                                          *input_contract_hash,
                                          *output_deployed_address);

  return {convert_return_t(res.first), res.second};
}
std::pair<HostFnResult, GasAmount>
syscall_GetSourceTransactionHash_outer(SystemCallHandler &syscall_handler,
                                       std::span<std::byte> contract_memory,
                                       uint64_t user_output_source_tx_hash,
                                       uint64_t /*unused*/, uint64_t /*unused*/,
                                       uint64_t /*unused*/, uint64_t /*unused*/,
                                       uint64_t /*unused*/)
{
  SHA256Hash *output_source_tx_hash =
      check_user_argument<SHA256Hash>(contract_memory,
                                      user_output_source_tx_hash,
                                      sizeof(SHA256Hash));

  if (!output_source_tx_hash) {
    return {{HostFnErrorCode::kNoneOrRecoverable,
             static_cast<int32_t>(SyscallErrorCode::kFault)},
            kGenericBadSyscallCost};
  }

  auto res = syscall_GetSourceTransactionHash_inner(syscall_handler,
                                                    *output_source_tx_hash);

  return {convert_return_t(res.first), res.second};
}
std::pair<HostFnResult, GasAmount>
syscall_Exit_outer(SystemCallHandler &syscall_handler,
                   std::span<std::byte> contract_memory,
                   uint64_t user_input_code, uint64_t /*unused*/,
                   uint64_t /*unused*/, uint64_t /*unused*/,
                   uint64_t /*unused*/, uint64_t /*unused*/)
{
  const int32_t input_code =
      check_user_primitive_argument<const int32_t>(user_input_code);

  auto res = syscall_Exit_inner(input_code);

  return {convert_return_t(res.first), res.second};
}

using SyscallFunc = std::pair<HostFnResult, uint64_t> (*)(SystemCallHandler &,
                                                          std::span<std::byte>,
                                                          uint64_t, uint64_t,
                                                          uint64_t, uint64_t,
                                                          uint64_t, uint64_t);

const std::array<std::pair<SyscallNo, SyscallFunc>, 12> syscall_table = {
    std::pair{SyscallNo::kSHA256, &syscall_SHA256_outer},
    std::pair{SyscallNo::kGetMsgSender, &syscall_GetMsgSender_outer},
    std::pair{SyscallNo::kGetCalldataLen, &syscall_GetCalldataLen_outer},
    std::pair{SyscallNo::kGetCalldata, &syscall_GetCalldata_outer},
    std::pair{SyscallNo::kLog, &syscall_Log_outer},
    std::pair{SyscallNo::kInvoke, &syscall_Invoke_outer},
    std::pair{SyscallNo::kReturn, &syscall_Return_outer},
    std::pair{SyscallNo::kGetReturnedBuffer, &syscall_GetReturnedBuffer_outer},
    std::pair{SyscallNo::kGetDeployAddress, &syscall_GetDeployAddress_outer},
    std::pair{SyscallNo::kDeployContract, &syscall_DeployContract_outer},
    std::pair{SyscallNo::kGetSourceTransactionHash,
              &syscall_GetSourceTransactionHash_outer},
    std::pair{SyscallNo::kExit, &syscall_Exit_outer},
};

// returns gas_consumed, syscall error>
std::pair<HostFnResult, uint64_t>
syscall_entry_inner(HostCallContext *call_context, uint64_t arg0, uint64_t arg1,
                    uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5,
                    uint64_t arg6)
{
  if (arg0 >= syscall_table.size()) {
    // invalid syscall
    return {{HostFnErrorCode::kNoneOrRecoverable,
             static_cast<int32_t>(SyscallErrorCode::kUnknownSyscall)},
            kUnknownSyscallCost};
  }

  static_assert(syscall_table.size() < INT32_MAX,
                "xdr enum overflow otherwise");

  auto [syscall_expect, syscall_fn_outer] = syscall_table[arg0];

  assert(syscall_expect == static_cast<SyscallNo>(arg0));

  std::span<std::byte> contract_memory = call_context->runtime->get_memory();

  assert(call_context->syscall_handler);

  return syscall_fn_outer(*call_context->syscall_handler, contract_memory, arg1,
                          arg2, arg3, arg4, arg5, arg6);
}

HostFnResult
syscall_entry(HostCallContext *call_context, uint64_t arg0, uint64_t arg1,
              uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5,
              uint64_t arg6)
{
  auto [err, gas_consumed] = syscall_entry_inner(call_context, arg0, arg1, arg2,
                                                 arg3, arg4, arg5, arg6);

  if (!call_context->runtime->consume_gas(gas_consumed)) {
    return {HostFnErrorCode::kOutOfGas, 0};
  }

  static_assert(std::same_as<std::underlying_type_t<SyscallErrorCode>, int32_t>,
                "mismatch");

  return err;
}

} // namespace hogd
