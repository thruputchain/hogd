#pragma once

/**
 * Copyright 2023 Geoffrey Ramseyer
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

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

#include <algorithm>
#include <cassert>
#include <iostream>
#include <iterator>
#include <memory>
#include <span>
#include <string>
#include <tuple>
#include <type_traits>
#include <vector>

#include "wasm3.h"

#include "hogd/hogexec/runtime/status.h"
#include "hogd/hogexec/runtime_trampoline/ffi_trampolines.h"

namespace hogd {

struct HostCallContext;

} // namespace hogd

namespace wasm3 {
/** @cond */
namespace detail {

static thread_local int32_t wasm3_err_code = 0;

typedef uint64_t *stack_type;
typedef void *mem_type;
template<typename T, typename...> struct first_type {
  typedef T type;
};

typedef const void *(*m3_api_raw_fn)(IM3Runtime, uint64_t *, void *);

template<typename T>
void
arg_from_stack(T &dest, stack_type &_sp, mem_type mem)
{
  m3ApiGetArg(T, tmp);
  dest = tmp;
}

template<typename T>
void
arg_from_stack(T *&dest, stack_type &_sp, mem_type _mem)
{
  m3ApiGetArgMem(T *, tmp);
  dest = tmp;
};

template<typename T>
void
arg_from_stack(const T *&dest, stack_type &_sp, mem_type _mem)
{
  m3ApiGetArgMem(const T *, tmp);
  dest = tmp;
};

template<typename... Args>
static void
get_args_from_stack(stack_type &sp, mem_type mem, std::tuple<Args...> &tuple)
{
  std::apply([&](auto &...item) { (arg_from_stack(item, sp, mem), ...); },
             tuple);
}

template<std::same_as<uint64_t>... Args>
const void *
wrap_fn_return(IM3Runtime rt, IM3ImportContext _ctx, stack_type _sp,
               mem_type mem)
{
  m3ApiReturnType(int32_t);

  using args_tuple_t = std::tuple<Args...>;

  args_tuple_t args;

  get_args_from_stack(_sp, mem, args);

  void *fn_pointer = _ctx->userdata;

  constexpr size_t nargs = std::tuple_size_v<args_tuple_t>;

  hogd::detail::TrampolineResult result;

  if constexpr (nargs == 0) {
    result = hogd::detail::c_call_0args(fn_pointer, m3_GetUserData(rt));
  }
  else if constexpr (nargs == 1) {
    result = hogd::detail::c_call_1args(fn_pointer, m3_GetUserData(rt),
                                        std::get<0>(args));
  }
  else if constexpr (nargs == 2) {
    result = hogd::detail::c_call_2args(fn_pointer, m3_GetUserData(rt),
                                        std::get<0>(args), std::get<1>(args));
  }
  else if constexpr (nargs == 3) {
    result = hogd::detail::c_call_3args(fn_pointer, m3_GetUserData(rt),
                                        std::get<0>(args), std::get<1>(args),
                                        std::get<2>(args));
  }
  else if constexpr (nargs == 4) {
    result = hogd::detail::c_call_4args(fn_pointer, m3_GetUserData(rt),
                                        std::get<0>(args), std::get<1>(args),
                                        std::get<2>(args), std::get<3>(args));
  }
  else if constexpr (nargs == 5) {
    result = hogd::detail::c_call_5args(fn_pointer, m3_GetUserData(rt),
                                        std::get<0>(args), std::get<1>(args),
                                        std::get<2>(args), std::get<3>(args),
                                        std::get<4>(args));
  }
  else if constexpr (nargs == 6) {
    result = hogd::detail::c_call_6args(fn_pointer, m3_GetUserData(rt),
                                        std::get<0>(args), std::get<1>(args),
                                        std::get<2>(args), std::get<3>(args),
                                        std::get<4>(args), std::get<5>(args));
  }
  else if constexpr (nargs == 7) {
    result = hogd::detail::c_call_7args(fn_pointer, m3_GetUserData(rt),
                                        std::get<0>(args), std::get<1>(args),
                                        std::get<2>(args), std::get<3>(args),
                                        std::get<4>(args), std::get<5>(args),
                                        std::get<6>(args));
  }
  else if constexpr (nargs == 8) {
    result = hogd::detail::c_call_8args(fn_pointer, m3_GetUserData(rt),
                                        std::get<0>(args), std::get<1>(args),
                                        std::get<2>(args), std::get<3>(args),
                                        std::get<4>(args), std::get<5>(args),
                                        std::get<6>(args), std::get<7>(args));
  }
  else {
    std::terminate();
  }

  hogd::HostFnErrorCode code{result.panic};
  using enum hogd::HostFnErrorCode;

  int32_t ret_val = result.result;

  switch (code) {
  case kNoneOrRecoverable:
    m3ApiReturn(ret_val);
    std::terminate(); // assert that m3ApiReturn macro actually returns
  case kUnrecoverable:
    m3ApiTrap(m3Err_unrecoverableSystemError);
  case kOutOfGas:
    m3ApiTrap(m3Err_outOfGasError);
  case kExit:
    wasm3_err_code = result.result;
    m3ApiTrap(m3Err_hogdExit);
  case kReturn:
    wasm3_err_code = 0;
    m3ApiTrap(m3Err_hogdExit);
  default:
    m3ApiTrap(m3Err_unrecoverableSystemError);
  }
  std::terminate();
}

static bool
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

static void
throw_nondeterministic_errors(M3Result result)
{
  if (is_nondeterminisic_error(result)) {
    throw std::runtime_error("nondeterministic error" + std::string(result));
  }
}

} // namespace detail
/** @endcond */

static bool
static_link_nargs(IM3Module io_module, const char *const i_moduleName,
                  const char *const i_functionName,
                  void *function, // expects signature of
                                  // HostFnStatus<int32_t>(HostCallContext*,
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

inline static hogd::InvokeStatus<int32_t>
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

class module;
class runtime;
class function;

/**
 * Wrapper for WASM3 environment.
 *
 * Runtimes, modules are owned by an environment.
 */
class environment {
public:
  environment()
  {
    m_env.reset(m3_NewEnvironment(), m3_FreeEnvironment);
    if (m_env == nullptr) {
      throw std::bad_alloc();
    }
  }

  /**
   * Create new runtime
   *
   * @param stack_size_bytes  size of the WASM stack for this runtime
   * @return runtime object
   */
  std::unique_ptr<runtime> new_runtime(size_t stack_size_bytes, void *ctxp);

  /**
   * Parse a WASM module from binary data
   *
   * @param data  pointer to the start of the binary
   * @param size  size of the binary
   * @return module object
   */
  std::unique_ptr<module> parse_module(const uint8_t *data, size_t size);

protected:
  std::shared_ptr<struct M3Environment> m_env;
};

/**
 * Wrapper for the runtime, where modules are loaded and executed.
 */
class runtime {
public:
  /**
   * Load the module into runtime
   * @param mod  module parsed by environment::parse_module
   */
  bool __attribute__((warn_unused_result)) load(module &mod);

  /**
   * Get a function handle by name
   *
   * If the function is not found, returns nullopt
   * @param name  name of a function, c-string
   * @return function object
   */
  std::optional<function> find_function(const char *name);

  std::span<std::byte> get_memory();
  std::span<const std::byte> get_memory() const;

  friend class environment;

  runtime(const std::shared_ptr<M3Environment> &env, size_t stack_size_bytes,
          void *ctxp)
    : m_env(env)
  {
    m_runtime.reset(m3_NewRuntime(env.get(), stack_size_bytes, ctxp),
                    &m3_FreeRuntime);
    if (m_runtime == nullptr) {
      throw std::bad_alloc();
    }
  }

protected:
  /* runtime extends the lifetime of the environment */
  std::shared_ptr<M3Environment> m_env;
  std::shared_ptr<M3Runtime> m_runtime;
};

/**
 * Module object holds a webassembly module
 *
 * It can be constructed by parsing a WASM binary using
 * environment::parse_module. Functions can be linked to the loaded module. Once
 * constructed, modules can be loaded into the runtime.
 */
class module {
public:
  // expected signature: HostFnStatus<ret_type>(HostCallContext*, uint64t
  // repeated nargs)
  bool link_nargs(const char *module, const char *function_name,
                  void *function_pointer, uint8_t nargs);

  ~module()
  {
    // after loading, the Module object is owned by the Runtime (i.e. when
    // m_loaded=true) and this wrapper class can be dropped.
    if ((!m_loaded) && (m_module != nullptr)) {
      m3_FreeModule(m_module);
    }
  }

  friend class environment;
  friend class runtime;

  module(const uint8_t *data, size_t size,
         const std::shared_ptr<M3Environment> &env) :m_env(env),
      m_moduleRawData(data, data + size)
  {}

  bool __attribute__((warn_unused_result)) init() { return parse(); }

protected:
  module(module &&) = delete;
  module(module &) = delete;
  module &operator=(const module &) = delete;

  // do not reuse module object
  bool __attribute__((warn_unused_result)) parse()
  {
    if (m_module) {
      return false;
    }

    IM3Module p;
    M3Result err = m3_ParseModule(m_env.get(), &p, m_moduleRawData.data(),
                                  m_moduleRawData.size());
    detail::throw_nondeterministic_errors(err);
    if (err != m3Err_none) {
      return false;
    }

    if (m_loaded) {
      m3_FreeModule(m_module);
    }
    m_module = p;
    return true;
  }

  bool __attribute__((warn_unused_result)) load_into(IM3Runtime runtime)
  {
    M3Result err = m3_LoadModule(runtime, m_module);
    detail::throw_nondeterministic_errors(err);
    if (err != m3Err_none) {
      return false;
    }
    m_loaded = true;
    return true;
  }

  // m_env exists only to extend lifetime of environment
  std::shared_ptr<M3Environment> m_env;
  IM3Module m_module = nullptr;

  bool m_loaded = false;
  const std::vector<uint8_t> m_moduleRawData;
};

/**
 * Handle of a function. Can be obtained from runtime::find_function method by
 * name.
 */
class function {
public:
  template<std::same_as<uint64_t>... Args>
  hogd::InvokeStatus<int32_t> call(Args... args)
  {
    const void *arg_ptrs[] = {reinterpret_cast<const void *>(&args)...};
    M3Result res = m3_Call(m_func, sizeof...(args), arg_ptrs);

    if (res != m3Err_none) {
      return m3_err_to_invoke_status(res);
    }

    int32_t ret = 0;
    const void *ret_ptrs[] = {&ret};
    res = m3_GetResults(m_func, 1, ret_ptrs);

    if (res == m3Err_argumentCountMismatch) {
      return hogd::InvokeError{hogd::InvokeErrorCode::kDeterministic};
    }

    if (res == m3Err_none) {
      return ret;
    }

    return hogd::InvokeError{hogd::InvokeErrorCode::kUnrecoverable};
  }

  friend class runtime;

  function(const std::shared_ptr<M3Runtime> &runtime) : m_runtime(runtime) {}

  bool __attribute__((warn_unused_result)) initialize(const char *name)
  {
    M3Result err = m3_FindFunction(&m_func, m_runtime.get(), name);
    detail::throw_nondeterministic_errors(err);
    return (m_func != nullptr);
  }

protected:
  std::shared_ptr<M3Runtime> m_runtime;
  M3Function *m_func = nullptr;
};

inline std::unique_ptr<runtime>
environment::new_runtime(size_t stack_size_bytes, void *ctxp)
{
  return std::make_unique<runtime>(m_env, stack_size_bytes, ctxp);
}

inline std::unique_ptr<module>
environment::parse_module(const uint8_t *data, size_t size)
{
  auto out = std::make_unique<module>(data, size, m_env);
  if (out->init()) {
    return out;
  }
  return nullptr;
}

inline bool __attribute__((warn_unused_result))
runtime::load(module &mod)
{
  return mod.load_into(m_runtime.get());
}

inline std::optional<function>
runtime::find_function(const char *name)
{
  auto f = function(m_runtime);
  if (!f.initialize(name)) {
    return std::nullopt;
  }
  return f;
}

inline std::span<std::byte>
runtime::get_memory()
{
  uint32_t len = 0;
  uint8_t *mem = m3_GetMemory(m_runtime.get(), &len, 0);
  return std::span<std::byte>{reinterpret_cast<std::byte *>(mem), len};
}

inline std::span<const std::byte>
runtime::get_memory() const
{
  uint32_t len = 0;
  const uint8_t *mem = m3_GetMemory(m_runtime.get(), &len, 0);
  return std::span<const std::byte>{reinterpret_cast<const std::byte *>(mem),
                                    len};
}

// expected signature: HostFnStatus<int32_t>(HostCallContext*, uint64t repeated
// nargs)
inline bool
module::link_nargs(const char *module, const char *function_name,
                   void *function_pointer, uint8_t nargs)
{
  return static_link_nargs(m_module, module, function_name, function_pointer,
                           nargs);
}

} // namespace wasm3
