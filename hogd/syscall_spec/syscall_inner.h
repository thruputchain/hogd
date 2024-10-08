#pragma once

#include <utility>

#include "hogd/base/types.h"
#include "hogd/hogexec/runtime/status.h"
#include "storage_object.hh"
#include "syscall.hh"
#include "types.hh"

namespace hogd {

class SystemCallHandler;

std::pair<HostFnStatus<SyscallErrorCode>, GasAmount>
syscall_SHA256_inner(std::span<const std::byte> input_buffer,
                     SHA256Hash &output_hash);

std::pair<HostFnStatus<SyscallErrorCode>, GasAmount>
syscall_GetMsgSender_inner(SystemCallHandler &syscall_handler,
                           ContractAddress &output_sender);

std::pair<HostFnStatus<SyscallErrorCode>, GasAmount>
syscall_BytestringGetLen_inner(SystemCallHandler &syscall_handler,
                               const StorageKey &input_key,
                               RuntimeMemoryLength &output_length);

std::pair<HostFnStatus<SyscallErrorCode>, GasAmount>
syscall_BytestringGet_inner(SystemCallHandler &syscall_handler,
                            const StorageKey &input_key,
                            std::span<std::byte> output_bytestring,
                            RuntimeMemoryLength &output_written_len);

std::pair<HostFnStatus<SyscallErrorCode>, GasAmount>
syscall_BytestringSet_inner(SystemCallHandler &syscall_handler,
                            const StorageKey &input_key,
                            std::span<const std::byte> input_bytestring);

std::pair<HostFnStatus<SyscallErrorCode>, GasAmount>
syscall_HasKey_inner(SystemCallHandler &syscall_handler,
                     const StorageKey &input_key,
                     StorageObjectType &output_status);

std::pair<HostFnStatus<SyscallErrorCode>, GasAmount>
syscall_DeleteLast_inner(SystemCallHandler &syscall_handler,
                         const StorageKey &input_key);

std::pair<HostFnStatus<SyscallErrorCode>, GasAmount>
syscall_GetCalldataLen_inner(SystemCallHandler &syscall_handler,
                             RuntimeMemoryLength &output_len);

std::pair<HostFnStatus<SyscallErrorCode>, GasAmount>
syscall_GetCalldata_inner(SystemCallHandler &syscall_handler,
                          std::span<std::byte> output_buffer,
                          RuntimeMemoryLength &output_buffer_written_len);

std::pair<HostFnStatus<SyscallErrorCode>, GasAmount>
syscall_Log_inner(SystemCallHandler &syscall_handler,
                  std::span<const std::byte> input_buffer);

} // namespace hogd
