#pragma once

#include <utility>

#include "hogd/base/types.h"
#include "hogd/hogexec/runtime/status.h"
#include "syscall.hh"
#include "types.hh"

namespace hogd {

class SystemCallHandler;

// NOLINTBEGIN(readability-identifier-naming)

std::pair<HostFnStatus<SyscallErrorCode>, GasAmount>
syscall_SHA256_inner(std::span<const std::byte> input_buffer,
                     SHA256Hash &output_hash);

std::pair<HostFnStatus<SyscallErrorCode>, GasAmount>
syscall_GetMsgSender_inner(SystemCallHandler &syscall_handler,
                           ContractAddress &output_sender);

std::pair<HostFnStatus<SyscallErrorCode>, GasAmount>
syscall_GetCalldataLen_inner(SystemCallHandler &syscall_handler,
                             RuntimeMemoryLength &output_len);

std::pair<HostFnStatus<SyscallErrorCode>, GasAmount>
syscall_GetCalldata_inner(SystemCallHandler &syscall_handler,
                          std::span<std::byte> output_buffer,
                          RuntimeMemoryLength &output_written_len);

std::pair<HostFnStatus<SyscallErrorCode>, GasAmount>
syscall_Log_inner(SystemCallHandler &syscall_handler,
                  std::span<const std::byte> input_buffer);

std::pair<HostFnResult, GasAmount>
syscall_Invoke_inner(SystemCallHandler &syscall_handler,
                     const ContractAddress &input_address,
                     MethodID input_method, GasAmount input_gas_limit,
                     std::span<const std::byte> input_calldata,
                     RuntimeMemoryLength &output_return_len);

std::pair<HostFnStatus<SyscallErrorCode>, GasAmount>
syscall_Return_inner(SystemCallHandler &syscall_handler,
                     std::span<const std::byte> input_buffer);

std::pair<HostFnStatus<SyscallErrorCode>, GasAmount>
syscall_GetReturnedBuffer_inner(SystemCallHandler &syscall_handler,
                                std::span<std::byte> output_buffer,
                                RuntimeMemoryLength &output_written_len);

std::pair<HostFnStatus<SyscallErrorCode>, GasAmount>
syscall_GetDeployAddress_inner(SystemCallHandler &syscall_handler,
                               const SHA256Hash &input_nonce,
                               const SHA256Hash &input_contract_hash,
                               ContractAddress &output_to_be_deployed);

std::pair<HostFnStatus<SyscallErrorCode>, GasAmount>
syscall_DeployContract_inner(SystemCallHandler &syscall_handler,
                             const SHA256Hash &input_nonce,
                             const SHA256Hash &input_contract_hash,
                             ContractAddress &output_deployed_address);

std::pair<HostFnStatus<SyscallErrorCode>, GasAmount>
syscall_GetSourceTransactionHash_inner(SystemCallHandler &syscall_handler,
                                       SHA256Hash &output_source_tx_hash);

std::pair<HostFnResult, GasAmount> syscall_Exit_inner(int32_t input_code);

// NOLINTEND(readability-identifier-naming)

} // namespace hogd
