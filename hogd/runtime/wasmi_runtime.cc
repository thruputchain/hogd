#include "hogd/runtime/wasmi_runtime.h"

#include "hogd/base/types.h"

#include "hogd/runtime/host_fn_status.h"

#include <span>
#include <string>
#include <cassert>

namespace hogd {

namespace detail {

// FFI declarations
extern "C" {

/**
 * Key rust FFI rules:
 * 1) exported functions are #[no_mangle]
 * 2) exported types are #[repr(C)]
 * 3) void* -> *mut c_void, const void* -> *const c_void
 */

/*
#[no_mangle]
pub fn new_wasmi_runtime(
    bytes: *const u8,
    bytes_len: u32,
    userctx: *mut c_void,
) -> *mut WasmiRuntime;
*/

void *new_wasmi_runtime(const uint8_t *script_ptr, uint32_t script_len,
                        void *user_ctx_ptr);

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
    error: u32,
} */
struct WasmiInvokeResult {
  uint64_t result;
  uint32_t error;
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
pub extern "C" fn wasmi_get_available_gas(runtime: *const WasmiRuntime) -> u64
*/
uint64_t wasmi_get_available_gas(const void *wasmi_runtime_ptr);

/*
#[no_mangle]
pub extern "C" fn wasmi_set_available_gas(runtime: *mut WasmiRuntime, gas : u64)
*/
void wasmi_set_available_gas(void *wasmi_runtime_ptr, uint64_t gas);

/*
#[no_mangle]
pub extern "C" fn wasmi_link_nargs(
    runtime: *mut WasmiRuntime,
    module_name: *const u8,
    module_name_len: u32,
    method_name: *const u8,
    method_name_len: u32,
    function_pointer: *mut c_void,
    nargs: u8,
) -> bool;
*/
bool wasmi_link_nargs(void *wasmi_runtime_ptr, const uint8_t *module_name,
                      uint32_t module_name_len, const uint8_t *method_name,
                      uint32_t method_name_len, void *fn_ptr, uint8_t n_args);
/*
#[no_mangle]
pub extern "C" fn wasmi_invoke(
    runtime: *mut Wasmi_WasmRuntime,
    bytes: *const u8,
    bytes_len: u32,
) -> WasmiInvokeResult
*/

WasmiInvokeResult
wasmi_invoke(void* wasmi_runtime_ptr,
  const uint8_t* method_name,
  uint32_t method_name_len);

} // extern C

} // namespace detail

std::span<uint8_t, std::dynamic_extent>
WasmiRuntime::get_memory()
{
  auto slice = detail::wasmi_get_memory(wasmi_runtime_ptr_);
  return std::span<uint8_t, std::dynamic_extent>(slice.mem, slice.sz);
}

std::span<const uint8_t, std::dynamic_extent>
WasmiRuntime::get_memory() const
{
  auto slice = detail::wasmi_get_memory(wasmi_runtime_ptr_);
  return std::span<uint8_t, std::dynamic_extent>(slice.mem, slice.sz);
}

bool
WasmiRuntime::link_fn_nargs(std::string const &module_name,
                            std::string const &fn_name, void *fn_ptr,
                            uint8_t nargs)
{
  return detail::wasmi_link_nargs(
      wasmi_runtime_ptr_,
      reinterpret_cast<const uint8_t *>(module_name.c_str()),
      module_name.size(), reinterpret_cast<const uint8_t *>(fn_name.c_str()),
      fn_name.size(), fn_ptr, nargs);
}

InvokeResult 
WasmiRuntime::invoke(std::string const& method_name,
    uint64_t gas_limit)
{
  uint64_t prev_gas_limit = detail::wasmi_get_available_gas(wasmi_runtime_ptr_);
  detail::wasmi_set_available_gas(wasmi_runtime_ptr_, gas_limit);

  detail::WasmiInvokeResult res = detail::wasmi_invoke(
    wasmi_runtime_ptr_,
    reinterpret_cast<const uint8_t*>(method_name.c_str()),
    method_name.size());


  uint64_t gas_remaining = detail::wasmi_get_available_gas(wasmi_runtime_ptr_);

  assert(gas_remaining <= gas_limit);

  uint64_t gas_consumed = gas_limit - gas_remaining;

  detail::wasmi_set_available_gas(wasmi_runtime_ptr_, prev_gas_limit);

  WasmiInvokeError wasmi_error{res.error};

  if (wasmi_error != WasmiInvokeError::kWasmiNone)
  {
    return InvokeResult {
      .result = HostFnError{wasmi_to_host_fn_error(wasmi_error)},
      .gas_consumed = gas_consumed
    };
  }
  return InvokeResult{
    .result = res.result,
    .gas_consumed = gas_consumed
  };
}

WasmiRuntime::WasmiRuntime(Contract const &contract, void *user_ctx_ptr)
  : host_call_context_(this, user_ctx_ptr),
    wasmi_runtime_ptr_(detail::new_wasmi_runtime(contract.data(),
                                                 contract.size(),
                                                 &host_call_context_))
{}

WasmiRuntime::~WasmiRuntime()
{
  if (wasmi_runtime_ptr_) {
    detail::free_wasmi_runtime(wasmi_runtime_ptr_);
  }
}

} // namespace hogd
