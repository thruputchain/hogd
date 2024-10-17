#include <cassert>
#include <cstddef>
#include <format>
#include <memory>
#include <span>
#include <string_view>
#include <utility>

#include "hogd/base/types.h"
#include "hogd/hogexec/runtime/status.h"
#include "hogd/hogexec/runtime/wasmi_runtime.h"

#include "contract.hh"

namespace hogd {

namespace {

// FFI declarations
extern "C" {

// *** REQUIREMENT ***
// These functions are only used in this file, so as to make
// tracking these invariants easier.
// Also, big capital letters
// *** REQUIREMENT ***
// Argument passed in as wasmi_runtime_ptr is NEVER NULL

/*
 * Key rust FFI rules:
 * 1) exported functions are #[no_mangle]
 * 2) exported types are #[repr(C)]
 * 3) void* -> *mut c_void, const void* -> *const c_void
 */

// Stellar-core uses cxx::bindgen.  That would be a good package
// to use if this gets significantly more complicated,
// or we find ourselves constantly adjusting this API.

/*
#[no_mangle]
pub extern "C" fn new_wasmi_context() -> *mut c_void;
*/

void *new_wasmi_context();

/*
#[no_mangle]
pub extern "C" fn free_wasmi_context(p: *mut c_void);
*/

void free_wasmi_context(void *wasmi_context_ptr);

/*
#[no_mangle]
pub fn new_wasmi_runtime(
    bytes: *const u8,
    bytes_len: u32,
    userctx: *mut c_void,
    wasmi_context_ptr: *const c_void
) -> *mut WasmiRuntime;
*/

void *new_wasmi_runtime(const uint8_t *script_ptr, uint32_t script_len,
                        void *user_ctx_ptr, void *wasmi_context_ptr);

/*
#[no_mangle]
pub extern "C" fn free_wasmi_runtime(p: *mut WasmiRuntime);
*/

void free_wasmi_runtime(void *wasmi_runtime_ptr);

/*
#[repr(C)]
pub struct MemorySlice {
    mem: *mut u8,
    sz: u32,
}
*/

struct MemorySlice {
  uint8_t *mem;
  uint32_t sz;
};

/*
#[repr(C)]
pub struct WasmiInvokeResult {
    result: u64,
    error: u8,
} */
struct WasmiInvokeResult {
  int32_t result;
  uint8_t error; // InvokeErrorCode
};

/*
#[no_mangle]
pub extern "C" fn wasmi_get_memory(runtime: *mut WasmiRuntime) -> MemorySlice;
*/

MemorySlice wasmi_get_memory(void *wasmi_runtime_ptr);

/*
#[no_mangle]
pub extern "C" fn wasmi_consume_gas(runtime: *mut WasmiRuntime, gas_consumed:
u64) -> bool;
*/
bool wasmi_consume_gas(void *wasmi_runtime_ptr, uint64_t gas_consumed);
/*
#[no_mangle]
pub extern "C" fn wasmi_get_available_gas(runtime: *const c_void) -> u64
*/
uint64_t wasmi_get_available_gas(const void *wasmi_runtime_ptr);

/*
#[no_mangle]
pub extern "C" fn wasmi_set_available_gas(runtime: *mut c_void, gas : u64)
*/
void wasmi_set_available_gas(void *wasmi_runtime_ptr, uint64_t gas);

/*
#[no_mangle]
pub extern "C" fn wasmi_link_nargs(
    context: *mut c_void,
    module_name: *const u8,
    module_name_len: u32,
    method_name: *const u8,
    method_name_len: u32,
    function_pointer: *mut c_void,
    nargs: u8,
) -> bool;
*/
bool wasmi_link_nargs(void *wasmi_context_ptr, const uint8_t *module_name,
                      uint32_t module_name_len, const uint8_t *method_name,
                      uint32_t method_name_len, void *fn_ptr, uint8_t n_args);
/*
#[no_mangle]
pub extern "C" fn wasmi_invoke(
    runtime: *mut Wasmi_WasmRuntime,
    method_name: *const u8,
    method_name_len: u32,
) -> WasmiInvokeResult
*/

WasmiInvokeResult wasmi_invoke(void *wasmi_runtime_ptr,
                               const uint8_t *method_name,
                               uint32_t method_name_len);

} // extern C

} // namespace

WasmiContext::WasmiContext() : wasmi_context_ptr_(new_wasmi_context())
{}

WasmiContext::~WasmiContext()
{
  free_wasmi_context(wasmi_context_ptr_);
}

std::unique_ptr<WasmiRuntime>
WasmiContext::make_new_runtime(ContractBlob const &contract,
                               SystemCallHandler *syscall_handler)
{
  std::unique_ptr<HostCallContext> host_call_context =
      std::make_unique<HostCallContext>();
  host_call_context->syscall_handler = syscall_handler;

  void *wasmi_runtime_ptr =
      new_wasmi_runtime(contract.data(), contract.size(),
                        host_call_context.get(), wasmi_context_ptr_);

  if (wasmi_runtime_ptr == nullptr) {
    return {};
  }

  return std::make_unique<WasmiRuntime>(std::move(host_call_context),
                                        wasmi_runtime_ptr);
}

bool
WasmiContext::link_fn_nargs(std::string_view module_name,
                            std::string_view fn_name, void *fn_ptr,
                            uint8_t nargs)
{
  assert(wasmi_context_ptr_ != nullptr);
  return wasmi_link_nargs(wasmi_context_ptr_,
                          reinterpret_cast<const uint8_t *>(module_name.data()),
                          module_name.size(),
                          reinterpret_cast<const uint8_t *>(fn_name.data()),
                          fn_name.size(), fn_ptr, nargs);
}

std::span<std::byte>
WasmiRuntime::get_memory() noexcept
{
  assert(wasmi_runtime_ptr_ != nullptr);
  auto slice = wasmi_get_memory(wasmi_runtime_ptr_);
  return std::as_writable_bytes(std::span<uint8_t>(slice.mem, slice.sz));
}

std::span<const std::byte>
WasmiRuntime::get_memory() const noexcept
{
  assert(wasmi_runtime_ptr_ != nullptr);
  auto slice = wasmi_get_memory(wasmi_runtime_ptr_);
  return std::as_bytes(std::span<const uint8_t>(slice.mem, slice.sz));
}

InvokeResult
WasmiRuntime::invoke(uint32_t method_name, uint64_t gas_limit)
{
  auto method_name_str = std::format("{:x}", method_name);

  return invoke(method_name_str, gas_limit);
}

InvokeResult
WasmiRuntime::invoke(std::string_view method_name, uint64_t gas_limit)
{
  assert(wasmi_runtime_ptr_ != nullptr);

  uint64_t prev_gas_limit = wasmi_get_available_gas(wasmi_runtime_ptr_);
  wasmi_set_available_gas(wasmi_runtime_ptr_, gas_limit);

  WasmiInvokeResult res =
      wasmi_invoke(wasmi_runtime_ptr_,
                   reinterpret_cast<const uint8_t *>(method_name.data()),
                   method_name.size());

  uint64_t gas_remaining = wasmi_get_available_gas(wasmi_runtime_ptr_);

  assert(gas_remaining <= gas_limit);

  uint64_t gas_consumed = gas_limit - gas_remaining;

  wasmi_set_available_gas(wasmi_runtime_ptr_, prev_gas_limit);

  InvokeErrorCode wasmi_error{res.error};

  if (wasmi_error != InvokeErrorCode::kNone) {
    return InvokeResult{.result = {InvokeError{wasmi_error}},
                        .gas_consumed = gas_consumed};
  }
  return InvokeResult{.result = res.result, .gas_consumed = gas_consumed};
}

bool
WasmiRuntime::consume_gas(uint64_t gas) noexcept
{
  assert(wasmi_runtime_ptr_ != nullptr);
  return wasmi_consume_gas(wasmi_runtime_ptr_, gas);
}

uint64_t
WasmiRuntime::get_available_gas() const
{
  assert(wasmi_runtime_ptr_ != nullptr);
  return wasmi_get_available_gas(wasmi_runtime_ptr_);
}

void
WasmiRuntime::set_available_gas(uint64_t gas)
{
  assert(wasmi_runtime_ptr_ != nullptr);
  wasmi_set_available_gas(wasmi_runtime_ptr_, gas);
}

WasmiRuntime::WasmiRuntime(std::unique_ptr<HostCallContext> host_call_context,
                           void *wasmi_runtime_ptr)
  : host_call_context_(host_call_context.release()),
    wasmi_runtime_ptr_(wasmi_runtime_ptr)
{
  host_call_context_->runtime = this;
}

WasmiRuntime::~WasmiRuntime()
{
  if (wasmi_runtime_ptr_ != nullptr) {
    free_wasmi_runtime(wasmi_runtime_ptr_);
  }
}

} // namespace hogd
