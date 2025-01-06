#include "hogd/hogexec/runtime/wasm3_wrapper.h"

#include <cassert>
#include <memory>
#include <span>
#include <string>
#include <tuple>
#include <type_traits>
#include <vector>
#include <stdexcept>

#include "wasm3.h"

#include "hogd/hogexec/runtime/status.h"
#include "hogd/hogexec/runtime_trampoline/ffi_trampolines.h"

/*
Based on wasm3/platforms/cpp/wasm3_cpp.h

Original Copyright Notice:

MIT License

Copyright (c) 2019 Steven Massey, Volodymyr Shymanskyy

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.

*/

namespace wasm3 {

namespace detail {
inline thread_local int32_t wasm3_err_code = 0;

bool
is_nondeterminisic_error(M3Result result)
{
  if (result == m3Err_mallocFailed) {
    return true;
  }
  if (result == m3Err_unrecoverableSystemError) {
    return true;
  }
  if (result == m3Err_mallocFailedCodePage) {
    return true;
  }
  return false;
}

void
throw_nondeterministic_errors(M3Result result)
{
  if (is_nondeterminisic_error(result)) {
    throw std::runtime_error("nondeterministic error" + std::string(result));
  }
}

} // namespace detail

hogd::InvokeStatus<int32_t>
m3_err_to_invoke_status(M3Result res)
{
  assert(res != m3Err_none);

  if (res == m3Err_outOfGasError) {
    return hogd::InvokeError{hogd::InvokeErrorCode::kOutOfGas};
  }
  if (detail::is_nondeterminisic_error(res)) {
    return hogd::InvokeError{hogd::InvokeErrorCode::kUnrecoverable};
  }
  if (res == m3Err_hogdExit) {
    return hogd::InvokeStatus<int32_t>{detail::wasm3_err_code};
  }

  // all other errors
  return hogd::InvokeError{hogd::InvokeErrorCode::kDeterministic};
}

bool
static_link_nargs(IM3Module io_module, const char *const i_moduleName,
                  const char *const i_functionName,
                  void *function, // expects signature of
                                  // HostFnResult(HostCallContext*,
                                  // nargs repeated uint64)
                  uint8_t nargs)
{
  auto arg_sig = [&nargs]() -> std::string {
    std::string out = "i(";
    for (auto i = 0u; i < nargs; i++) {
      out += "I";
    }
    out += ")";
    return out;
  };

  decltype(&detail::wrap_fn_return<>) wrapped_fn_pointer = nullptr;

  // gross but it works
  switch (nargs) {
  case 0:
    wrapped_fn_pointer = &detail::wrap_fn_return<>;
    break;
  case 1:
    wrapped_fn_pointer = &detail::wrap_fn_return<uint64_t>;
    break;
  case 2:
    wrapped_fn_pointer = &detail::wrap_fn_return<uint64_t, uint64_t>;
    break;
  case 3:
    wrapped_fn_pointer = &detail::wrap_fn_return<uint64_t, uint64_t, uint64_t>;
    break;
  case 4:
    wrapped_fn_pointer =
        &detail::wrap_fn_return<uint64_t, uint64_t, uint64_t, uint64_t>;
    break;
  case 5:
    wrapped_fn_pointer = &detail::wrap_fn_return<uint64_t, uint64_t, uint64_t,
                                                 uint64_t, uint64_t>;
    break;
  case 6:
    wrapped_fn_pointer = &detail::wrap_fn_return<uint64_t, uint64_t, uint64_t,
                                                 uint64_t, uint64_t, uint64_t>;
    break;
  case 7:
    wrapped_fn_pointer =
        &detail::wrap_fn_return<uint64_t, uint64_t, uint64_t, uint64_t,
                                uint64_t, uint64_t, uint64_t>;
    break;
  case 8:
    wrapped_fn_pointer =
        &detail::wrap_fn_return<uint64_t, uint64_t, uint64_t, uint64_t,
                                uint64_t, uint64_t, uint64_t, uint64_t>;
    break;
  default:
    return false;
  }

  auto cur_sig = arg_sig();

  M3Result result =
      m3_LinkRawFunctionEx(io_module, i_moduleName, i_functionName,
                           cur_sig.c_str(), wrapped_fn_pointer, function);

  return (result == m3Err_none || result == m3Err_functionLookupFailed);
}

std::unique_ptr<Runtime>
Environment::new_runtime(size_t stack_size_bytes, void *ctxp)
{
  return std::make_unique<Runtime>(m3env_, stack_size_bytes, ctxp);
}

std::shared_ptr<Module>
Environment::parse_module(const uint8_t *data, size_t size)
{
  auto out = std::make_shared<Module>(data, size, m3env_);
  if (out->parse()) {
    return out;
  }
  return nullptr;
}

bool
Runtime::load(std::shared_ptr<Module> mod)
{
  bool load_result = mod->load_into(m3runtime_.get());
  if (load_result) {
    loaded_modules_.emplace_back(mod);
  }
  return load_result;
}

std::optional<Function>
Runtime::find_function(const char *name)
{
  auto f = Function(m3runtime_);
  if (!f.initialize(name)) {
    return std::nullopt;
  }
  return f;
}

std::span<std::byte>
Runtime::get_memory()
{
  uint32_t len = 0;
  uint8_t *mem = m3_GetMemory(m3runtime_.get(), &len, 0);
  return std::span<std::byte>{reinterpret_cast<std::byte *>(mem), len};
}

std::span<const std::byte>
Runtime::get_memory() const
{
  uint32_t len = 0;
  const uint8_t *mem = m3_GetMemory(m3runtime_.get(), &len, 0);
  return std::span<const std::byte>{reinterpret_cast<const std::byte *>(mem),
                                    len};
}

bool
Module::parse()
{
  // do not reuse the module object,
  // or attempt to parse twice.
  if (m3module_) {
    return false;
  }
  assert(!loaded_);

  IM3Module p;
  M3Result err = m3_ParseModule(m3env_.get(), &p, module_raw_data_.data(),
                                module_raw_data_.size());
  detail::throw_nondeterministic_errors(err);
  if (err != m3Err_none) {
    return false;
  }

  m3module_ = p;
  return true;
}

bool
Module::load_into(IM3Runtime runtime)
{
  M3Result err = m3_LoadModule(runtime, m3module_);
  detail::throw_nondeterministic_errors(err);
  if (err != m3Err_none) {
    return false;
  }
  loaded_ = true;
  return true;
}

bool
Module::link_nargs(const char *module_name, const char *function_name,
                   void *function_pointer, uint8_t nargs)
{
  return static_link_nargs(m3module_, module_name, function_name,
                           function_pointer, nargs);
}

bool
Function::initialize(const char *name)
{
  M3Result err = m3_FindFunction(&m3func_, m3runtime_.get(), name);
  detail::throw_nondeterministic_errors(err);
  return (m3func_ != nullptr);
}

} // namespace wasm3
