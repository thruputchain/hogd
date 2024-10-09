#!/bin/python3

import yaml

from yaml import CLoader as Loader, CDumper as Dumper

import argparse

def gen_validate_arg(inp, *, is_input):
  arg_name = inp["name"]
  const = ""
  in_out = "output"
  if (is_input):
    const = "const "
    in_out = "input"


  if not inp["fixed_size"]:
    arg1 = f"{in_out}_{arg_name}"
    arg2 = f"{in_out}_{arg_name}_len"
    return f"""
  std::optional<std::span<{const}std::byte>> {arg1} =
      check_user_span_argument(contract_memory, user_{arg1},
                               user_{arg2});

  if (!{in_out}_{arg_name}) {{
    return {{SyscallErrorCode::kFault, kGenericBadSyscallCost}};
  }}
""", [f"user_{arg1}", f"user_{arg2}"], arg1, f"*{arg1}", f"std::span<{const}std::byte> {arg1}"
  

  typename = inp["typename"]
  arg1 = f"{in_out}_{arg_name}"
  
  if "primitive_type" in inp and inp["primitive_type"] and is_input:
    return f"""
  {const}{typename} {arg1} =
      check_user_primitive_argument<{const}{typename}>(user_{arg1});
""", [f"user_{arg1}"], arg1, arg1, f"{const}{typename} {arg1}"

  # else fixed size
  return f"""
  {const}{typename} *{arg1} =
      check_user_argument<{const}{typename}>(contract_memory, user_{arg1},
                                      sizeof({typename}));

  if (!{arg1}) {{
    return {{SyscallErrorCode::kFault, kGenericBadSyscallCost}};
  }}
""", [f"user_{arg1}"], arg1,  f"*{arg1}", f"{const}{typename}& {arg1}"


def gen_inner_outer(syscall):
  name = syscall["name"]
  arg1 = "/*unused*/"
  arg2 = "/*unused*/"
  arg3 = "/*unused*/"
  arg4 = "/*unused*/"
  arg5 = "/*unused*/"
  arg6 = "/*unused*/"
  arg7 = "/*unused*/"

  inner_body = ""
  call_arg_strs = []
  user_args = []
  inner_arg_decl_strs = []
  syscall_handler = ""
  syscall_handler_decl = ""
  if syscall["inputs"] is not None:
    for arg in syscall["inputs"]:
      validate_str, local_user_args, local_arg, local_call_arg, inner_arg_decl = gen_validate_arg(arg, is_input = True)
      inner_body += validate_str
      user_args += local_user_args
      call_arg_strs.append(local_call_arg)
      inner_arg_decl_strs.append(inner_arg_decl)

  if syscall["outputs"] is not None:
    for arg in syscall["outputs"]:
      validate_str, local_user_args, local_arg, local_call_arg, inner_arg_decl = gen_validate_arg(arg, is_input = False)
      inner_body += validate_str
      user_args += local_user_args
      call_arg_strs.append(local_call_arg)
      inner_arg_decl_strs.append(inner_arg_decl)
      

      if "use_written_len" in arg and arg["use_written_len"]:
        local_name = arg["name"]
        writelen_arg = {
          "name" : f"""{local_name}_written_len""",
          "fixed_size" : True,
          "typename" : "RuntimeMemoryLength",
          "primitive_type": True
        }
        validate_str, local_user_args, local_arg, inner_arg_decl = gen_validate_arg(writelen_arg, is_input = False)
        inner_body += validate_str
        user_args += local_user_args
        call_arg_strs.append(f"*{local_arg}")
        inner_arg_decl_strs.append(inner_arg_decl)


  if len(user_args) >= 1:
    arg1 = user_args[0]
  if len(user_args) >= 2:
    arg2 = user_args[1]
  if len(user_args) >= 3:
    arg3 = user_args[2]
  if len(user_args) >= 4:
    arg4 = user_args[3] 
  if len(user_args) >= 5:
    arg5 = user_args[4]
  if len(user_args) >= 6:
    arg6 = user_args[5]
  if len(user_args) >= 7:
    arg7 = user_args[6]
  if len(user_args) >= 8:
    raise ValueError("too many syscall args")

  if syscall["needs_context"]:
    syscall_handler = "syscall_handler, "
    syscall_handler_decl = "SystemCallHandler& syscall_handler, "


  inner_args = ", ".join(call_arg_strs)
  inner_arg_decls = ", ".join(inner_arg_decl_strs);
  outer= f"""std::pair<HostFnStatus<SyscallErrorCode>, GasAmount>
syscall_{name}_outer(SystemCallHandler& syscall_handler,
                     std::span<std::byte> contract_memory,
                     uint64_t {arg1}, 
                     uint64_t {arg2},
                     uint64_t {arg3}, 
                     uint64_t {arg4},
                     uint64_t {arg5},
                     uint64_t {arg6},
                     uint64_t {arg7})
{{{inner_body}
  return syscall_{name}_inner({syscall_handler}{inner_args});
}}
"""
  inner = f"""std::pair<HostFnStatus<SyscallErrorCode>, GasAmount>
syscall_{name}_inner({syscall_handler_decl}{inner_arg_decls});

"""
  return outer, inner

def gen_outer(syscall):
  return gen_inner_outer(syscall)[0]

def gen_inner(syscall):
  return gen_inner_outer(syscall)[1]


def gen_syscall_table_entry(syscall):
  name = syscall["name"]
  return f"""    std::pair{{SyscallNo::k{name}, &syscall_{name}_outer}},
"""

def gen_syscall_table(spec):
  counter = 0
  max_seen = -1
  syscall_map = {}
  for syscall in spec:
    number = syscall["number"]
    if (number >= max_seen):
      max_seen = number + 1
    syscall_map[number] = syscall

  if max_seen != len(syscall_map):
    raise Exception("SyscallNo Gap!")
  table = f"""const std::array<std::pair<SyscallNo, SyscallFunc>, {max_seen}> syscall_table = {{
"""
  for i in range(0, max_seen):
    table += gen_syscall_table_entry(syscall_map[i])
  table += "};"
  return table

def gen_syscall_entry_cc(spec):
  syscall_outers = ""
  for syscall in spec:
    syscall_outers += gen_outer(syscall)

  syscall_table = gen_syscall_table(spec)

  return f"""#include "hogd/hogexec/runtime/syscall_entry.h"

#include <array>
#include <cassert>
#include <cstddef>
#include <cstring>
#include <optional>
#include <span>
#include <utility>
#include <type_traits>

#include "hogd/base/types.h"
#include "hogd/crypto/crypto.h"
#include "hogd/hogexec/runtime/gas_cost_table.h"
#include "hogd/hogexec/runtime/status.h"
#include "hogd/hogexec/runtime/syscall_inner.h"
#include "hogd/hogexec/runtime/wasmi_runtime.h"
#include "storage_object.hh"
#include "syscall.hh"
#include "types.hh"

namespace hogd {{

bool
check_memory_range(uint64_t memory_len, uint64_t base, uint64_t len)
{{
  uint64_t base_end = 0;
  if (__builtin_add_overflow(base, len, &base_end))
    return false;
  return (base_end <= memory_len);
}}

template<typename T> requires std::is_trivially_copyable_v<T>
T *
check_user_argument(std::span<std::byte> contract_memory, uint64_t user_offset,
                    uint64_t user_len)
{{
  if (!check_memory_range(contract_memory.size(), user_offset, user_len)) {{
    return nullptr;
  }}
  return reinterpret_cast<T *>(contract_memory.data() + user_offset);
}}

std::optional<std::span<std::byte>>
check_user_span_argument(std::span<std::byte> contract_memory,
                         uint64_t user_offset, uint64_t user_len)
{{
  if (!check_memory_range(contract_memory.size(), user_offset, user_len)) {{
    return std::nullopt;
  }}
  return contract_memory.subspan(user_offset, user_len);
}}

template<typename T> requires std::is_integral_v<T>
T
check_user_primitive_argument(uint64_t user_value)
{{
  return static_cast<T>(user_value);
}}

{syscall_outers}

using SyscallFunc = std::pair<HostFnStatus<SyscallErrorCode>, uint64_t> (*)(
    SystemCallHandler&, std::span<std::byte>, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);

{syscall_table}

// returns gas_consumed, syscall error>
std::pair<HostFnStatus<SyscallErrorCode>, uint64_t>
syscall_entry_inner(HostCallContext *call_context, uint64_t arg0, uint64_t arg1,
                    uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5, uint64_t arg6, uint64_t arg7)
{{
  if (arg0 >= syscall_table.size()) {{
    // invalid syscall
    return {{SyscallErrorCode::kUnknownSyscall, kUnknownSyscallCost}};
  }}

  static_assert(syscall_table.size() < INT32_MAX,
                "xdr enum overflow otherwise");

  auto [syscall_expect, syscall_fn_outer] = syscall_table[arg0];

  assert(syscall_expect == static_cast<SyscallNo>(arg0));

  std::span<std::byte> contract_memory = call_context->runtime->get_memory();

  assert(call_context -> syscall_handler);

  return syscall_fn_outer(*call_context->syscall_handler, contract_memory, arg1, arg2, arg3, arg4, arg5, arg6, arg7);
}}

HostFnStatus<int32_t>
syscall_entry(HostCallContext *call_context, uint64_t arg0, uint64_t arg1,
              uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5, 
              uint64_t arg6, uint64_t arg7)
{{
  auto [err, gas_consumed] =
      syscall_entry_inner(call_context, arg0, arg1, arg2, arg3, arg4, arg5, arg6, arg7);

  if (!call_context->runtime->consume_gas(gas_consumed)) {{
    return HostFnError{{HostFnErrorCode::kOutOfGas}};
  }}

  static_assert(std::same_as<std::underlying_type_t<SyscallErrorCode>, int32_t>, "mismatch");

  return err.and_then([](SyscallErrorCode err) -> HostFnStatus<int32_t> {{ return static_cast<int32_t>(err); }});
}}

}} // namespace hogd
"""

def gen_syscall_inner_h(spec):
  inner_decls=""

  for syscall in spec:
    inner_decls += gen_inner(syscall)

  return f"""#pragma once

#include <utility>

#include "hogd/base/types.h"
#include "hogd/hogexec/runtime/status.h"
#include "storage_object.hh"
#include "syscall.hh"
#include "types.hh"

namespace hogd {{

class SystemCallHandler;

{inner_decls}}}// namespace hogd
"""


def gen_syscall_x(spec_full):
  spec = spec_full["syscalls"]
  syscalls = {}
  errors = {}
  for syscall in spec:
    syscalls[syscall["number"]]=syscall["name"]

  enumdecls = ""

  for i in range(0, len(syscalls)):
    name = syscalls[i]
    enumdecls += f"    k{name} = {i},\n"

  error_spec = spec_full["errors"]

  for error in error_spec:
    errors[error["number"]]=error

  error_enumdecls = ""

  for i in range(0, len(errors)):
    name = errors[i]["name"]
    docstring=errors[i]["doc"]
    error_enumdecls += f"\n    // {docstring}\n    k{name} = {i},\n"


  return f"""
%#include "types.hh"

namespace hogd {{

enum SyscallNo {{
{enumdecls}}};

enum SyscallErrorCode {{{error_enumdecls}}};

}} // namespace hogd
"""

def gen_runtime_status_cc(template_filename, spec_full):
  error_spec = spec_full["errors"]
  syscall_status_switch_decls = ""

  for error in error_spec:
    errname = error["name"]
    syscall_status_switch_decls += f"""  case k{errname}:
    return "k{errname}";
"""
  with open(template_filename, 'r') as file:
    return file.read().format(**locals())

def gen_rs_syscall_binding(syscall):
  name = syscall["name"]
  inputdecls = []
  syscallargs = []
  uninit_args = []
  if syscall["inputs"] is not None:
    for inp in syscall["inputs"]:
      input_name = inp["name"]
      input_type = "[u8]"
      if "primitive_type" in inp and inp["primitive_type"]:
        input_type = inp["typename"]
        syscallargs.append(f"""{input_name} as u64""")
        inputdecls.append(f"""{input_name} : {input_type}""")
      elif inp["fixed_size"]:
        input_type = inp["typename"]
        syscallargs.append(f"""{input_name}.as_ptr() as u64""")
        inputdecls.append(f"""{input_name} : &{input_type}""")
      else:
        syscallargs.append(f"""{input_name}.as_ptr() as u64""")
        syscallargs.append(f"""{input_name}.len() as u64""")
        inputdecls.append(f"""{input_name} : &{input_type}""")


      uninit_args.append(f"""{input_name}""")

  outputargs = []
  outputdecls = []
  outputinits = []

  uninit_inputdecls = list(inputdecls)

  if syscall["outputs"] is not None:
    for outp in syscall["outputs"]:
      output_name = outp["name"]
      output_type = "[u8]"
      uninit_outputtype = "[MaybeUninit<u8>]"
      if "primitive_type" in outp and outp["primitive_type"]:
        output_type = outp["typename"]
        syscallargs.append(f"""to_offset_mut({output_name}) as u64""")
        uninit_outputtype = output_type
        uninit_args.append(f"""{output_name}""")
      elif outp["fixed_size"]:
        output_type = outp["typename"]
        uninit_outputtype = f"""MaybeUninit<{output_type}>"""
        syscallargs.append(f"""{output_name}.as_mut_ptr() as u64""")
        uninit_args.append(f"""unsafe {{core::mem::transmute({output_name})}}""")
      else:
        syscallargs.append(f"""{output_name}.as_mut_ptr() as u64""")
        syscallargs.append(f"""{output_name}.len() as u64""")
        uninit_args.append(f"""unsafe {{core::mem::transmute({output_name})}}""")

      inputdecls.append(f"""{output_name} : &mut {output_type}""")
      uninit_inputdecls.append(f"""{output_name} : &mut {uninit_outputtype}""")

  inputdecl_str = ", ".join(inputdecls)
  uninit_inputdecl_str = ", ".join(uninit_inputdecls)

  syscall_argcount = 7
  if len(syscallargs) < syscall_argcount:
    syscallargs += ["0"] * (syscall_argcount - len(syscallargs))

  syscallargs_str = """,
      """.join(syscallargs)

  uninit_args_str = ", ".join(uninit_args)

  return f"""pub fn uninit_{name}({uninit_inputdecl_str}) -> Result<(), i32> {{

  // syscall will initialize outputs, if error is kNone
  let err =
    syscall(SyscallNo::k{name},
      {syscallargs_str});

  if err != SyscallErrorCode::kNone as i32 {{
    return Err(err);
  }}

  Ok(())
}}

pub fn raw_{name}({inputdecl_str}) -> Result<(), i32> {{
  uninit_{name}({uninit_args_str})
}}

"""

def gen_rs_syscall_bindings(spec):
  syscall_str = ""
  for syscall in spec:
    syscall_str += gen_rs_syscall_binding(syscall)

  return f"""#![allow(non_snake_case)]
use rs_xdr::*;
use crate::syscall::{{syscall, to_offset_mut}};
use core::mem::MaybeUninit;

{syscall_str}

"""




parser = argparse.ArgumentParser(description="syscall generator")

parser.add_argument('--syscall_x', action='store_true', help="gen syscall.x")
parser.add_argument('--syscall_inner_h', action='store_true', help="gen syscall_inner.h")
parser.add_argument('--syscall_entry_cc', action='store_true', help="gen syscall_entry_cc")
parser.add_argument('--syscall_yaml', type=str, required = True, help = "input yaml")
parser.add_argument('--syscall_rs', action='store_true', help = 'gen rust syscall bindings')
parser.add_argument('--status_cc', action='store_true', help="gen status.cc")
parser.add_argument('--template_filename', type=str, help = "template filename")
parser.add_argument('--output_dir', type=str, default = ".", help = "output folder")

args = parser.parse_args()

spec_full = yaml.load(open(args.syscall_yaml, 'r').read(), Loader=Loader)
spec = spec_full["syscalls"]

if (args.syscall_x):
  filename = args.output_dir + "/syscall.x"
  with open(filename, "w") as f:
    f.write(gen_syscall_x(spec_full))

if (args.syscall_inner_h):
  filename = args.output_dir + "/syscall_inner.h"
  with open(filename, "w") as f:
    f.write(gen_syscall_inner_h(spec))

if (args.syscall_entry_cc):
  filename = args.output_dir + "/syscall_entry.cc"
  with open(filename, "w") as f:
    f.write(gen_syscall_entry_cc(spec))

if (args.status_cc):
  filename = args.output_dir + "/status.cc"
  with open(filename, "w") as f:
    f.write(gen_runtime_status_cc(args.template_filename, spec_full))

if (args.syscall_rs):
  filename = args.output_dir + "/syscall_bindings.rs"
  with open(filename, "w") as f:
    f.write(gen_rs_syscall_bindings(spec))

