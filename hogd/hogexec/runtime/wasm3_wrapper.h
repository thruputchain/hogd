#pragma once

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
#include <iterator>
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

namespace hogd {

struct HostCallContext;

} // namespace hogd

namespace wasm3 {
namespace detail {

extern thread_local int32_t wasm3_err_code;

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

template<typename... Args>
void
get_args_from_stack(stack_type &sp, mem_type mem, std::tuple<Args...> &tuple)
{
  std::apply([&](auto &...item) { (arg_from_stack(item, sp, mem), ...); },
             tuple);
}

template<std::same_as<uint64_t>... Args>
const void *
wrap_fn_return(IM3Runtime rt, IM3ImportContext _ctx, stack_type _sp,
               mem_type mem) noexcept
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

} // namespace detail

class Module;
class Runtime;
class Function;

// Wrapper for WASM3 environment.
//
// Runtimes, modules are owned by an environment.
class Environment {
public:
  Environment()
  {
    m3env_.reset(m3_NewEnvironment(), m3_FreeEnvironment);
    if (m3env_ == nullptr) {
      throw std::bad_alloc();
    }
  }

  // Create new runtime
  //
  // stack_size_bytes is size of the WASM stack for this runtime.
  // ctxp is the context pointer supplied to any linked function.
  std::unique_ptr<Runtime> new_runtime(size_t stack_size_bytes, void *ctxp);

  // Parse a WASM module from binary data
  std::shared_ptr<Module> parse_module(const uint8_t *data, size_t size);

private:
  std::shared_ptr<struct M3Environment> m3env_;
};

// Wrapper for the runtime, where modules are loaded and executed.
class Runtime {
public:
  Runtime(const std::shared_ptr<M3Environment> &env, size_t stack_size_bytes,
          void *ctxp)
    : m3env_(env)
  {
    m3runtime_.reset(m3_NewRuntime(m3env_.get(), stack_size_bytes, ctxp),
                     &m3_FreeRuntime);
    if (m3runtime_ == nullptr) {
      throw std::bad_alloc();
    }
  }

  // Delete copy and move
  Runtime(const Runtime &) = delete;
  Runtime &operator=(const Runtime &) = delete;
  Runtime(const Runtime &&) = delete;
  Runtime &operator=(Runtime &&) = delete;

  // Load the module into runtime
  // mod shouuld be a module parsed by Environment::parse_module
  //
  // If this function returns true, then this object takes
  // ownership of module, and the module cannot be loaded
  // into another runtime.  The shared_ptr will be kept alive.
  //
  // wasm3 does not allow linking into a module
  // before the module is loaded.  Thus we use shared_ptr here,
  // instead of unique_ptr.
  [[nodiscard]] bool load(std::shared_ptr<Module> mod);

  // Get a function handle by name
  //
  // If the function is not found, returns nullopt
  // name is the name of the target function, as a c-string
  std::optional<Function> find_function(const char *name);

  std::span<std::byte> get_memory();
  std::span<const std::byte> get_memory() const;

private:
  // runtime extends the lifetime of the environment
  std::shared_ptr<M3Environment> m3env_;
  std::shared_ptr<M3Runtime> m3runtime_;
  std::vector<std::shared_ptr<Module>> loaded_modules_;
};

// Module object holds a webassembly module
//
// It can be constructed by parsing a WASM binary using
// environment::parse_module. Functions can be linked to the loaded module. Once
// constructed, modules can be loaded into the runtime.
//
// The module_ variable is a nullptr until parse() is called.
// Loading a module that has not been parsed into a runtime will always fail.
class Module {
public:
  Module(const uint8_t *data, size_t size,
         const std::shared_ptr<M3Environment> &env)
    : m3env_(env), module_raw_data_(data, data + size)
  {}

  // Delete copy and move
  Module(const Module &) = delete;
  Module &operator=(const Module &) = delete;
  Module(const Module &&) = delete;
  Module &operator=(Module &&) = delete;

  // expected signature: HostFnResult(HostCallContext*, uint64_t
  // repeated nargs)
  bool link_nargs(const char *module_name, const char *function_name,
                  void *function_pointer, uint8_t nargs);

  ~Module()
  {
    // after loading, the Module object is owned by the Runtime (i.e. when
    // loaded_=true) and this wrapper class can be dropped.
    // Loading also takes ownership of a reference to this object,
    // so that module_raw_data_ is not dropped early.
    if ((!loaded_) && (m3module_ != nullptr)) {
      m3_FreeModule(m3module_);
    }
  }

  friend class Environment;
  friend class Runtime;

private:
  [[nodiscard]] bool parse();

  [[nodiscard]] bool load_into(IM3Runtime runtime);

  // m_env exists only to extend lifetime of environment
  std::shared_ptr<M3Environment> m3env_;
  IM3Module m3module_ = nullptr;

  bool loaded_ = false;
  const std::vector<uint8_t> module_raw_data_;
};

// Handle of a function. Can be obtained from runtime::find_function method by
// name.
class Function {
public:
  Function(const std::shared_ptr<M3Runtime> &runtime) : m3runtime_(runtime) {}

  template<std::same_as<uint64_t>... Args>
  hogd::InvokeStatus<int32_t> call(Args... args)
  {
    if (!m3func_) {
      return hogd::InvokeError{hogd::InvokeErrorCode::kDeterministic};
    }

    const void *arg_ptrs[] = {reinterpret_cast<const void *>(&args)...};
    M3Result res = m3_Call(m3func_, sizeof...(args), arg_ptrs);

    if (res != m3Err_none)
      return m3_err_to_invoke_status(res);

    int32_t ret = 0;
    const void *ret_ptrs[] = {&ret};
    res = m3_GetResults(m3func_, 1, ret_ptrs);

    if (res != m3Err_none) 
      return m3_err_to_invoke_status(res);
    return ret;
  }

  friend class Runtime;

private:
  [[nodiscard]]
  bool initialize(const char *name);

  static hogd::InvokeStatus<int32_t>
  m3_err_to_invoke_status(M3Result res);

  std::shared_ptr<M3Runtime> m3runtime_;
  // Does not own the memory pointed to by m3func_.
  // The output of m3FindFunction, which sets this variable,
  // is valid for the lifetime of m3runtime_ (as per the wasm3 code).
  // Thus this object is freely copyable.
  M3Function *m3func_ = nullptr;
};

} // namespace wasm3
