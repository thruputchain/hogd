#!/bin/python3

import yaml

from yaml import CLoader as Loader, CDumper as Dumper

import argparse

syscall_argcount = 6

def get_check_arg_statement(arg):
  return f"""if (!{arg}) {{
    return {{{{HostFnErrorCode::kNoneOrRecoverable, static_cast<int32_t>(SyscallErrorCode::kFault)}}, kGenericBadSyscallCost}};
  }}"""

def gen_validate_arg(inp, *, is_input):
  arg_name = inp["name"]
  const = ""
  in_out = "output"
  if (is_input):
    const = "const "
    in_out = "input"

  optional = False
  if "optional" in inp and inp["optional"]:
    optional = True

  if "span_argument" in inp and inp["span_argument"]:
    arg1 = f"{in_out}_{arg_name}"
    arg2 = f"{in_out}_{arg_name}_len"
    if optional:
      return f"""
        std::optional<std::span<{const}std::byte>> {arg1} =
            check_user_span_argument(contract_memory, user_{arg1},
                                     user_{arg2});

        
      """, [f"user_{arg1}", f"user_{arg2}"], arg1, arg1, f"std::optional<std::span<{const}std::byte>> {arg1}"

    else:

      check_arg_statement = get_check_arg_statement(arg1)

      return f"""
  std::optional<std::span<{const}std::byte>> {arg1} =
      check_user_span_argument(contract_memory, user_{arg1},
                               user_{arg2});

  {check_arg_statement}
""", [f"user_{arg1}", f"user_{arg2}"], arg1, f"*{arg1}", f"std::span<{const}std::byte> {arg1}"
  # endif span_argument

  typename = inp["typename"]
  arg1 = f"{in_out}_{arg_name}"

  # put optional_ on front of arg name, for readability
  if optional:
    arg1 = f"optional_{arg1}"

  check_arg_statement = get_check_arg_statement(arg1)
  
  if "primitive_type" in inp and inp["primitive_type"] and is_input:
    
    if optional:
      raise Exception("optional primitive_type is unimplemented");

    return f"""
  {const}{typename} {arg1} =
      check_user_primitive_argument<{const}{typename}>(user_{arg1});
""", [f"user_{arg1}"], arg1, arg1, f"{typename} {arg1}"

  if "custom_parsing" in inp and inp["custom_parsing"] and is_input:
    if optional:
      raise Exception("optional parsing is a strict subset of custom parsing--don't use both")
    num_args = inp["custom_args_required"]
    custom_args = []
    for i in range(0, num_args):
      custom_args.append(f"user_{in_out}_{arg_name}_{i}")
    arg_str = ", ".join(custom_args)

    return f"""
  std::optional<{const}{typename}> {arg1} =
      check_user_custom_input_argument<{typename}>(contract_memory, {arg_str});

  {check_arg_statement}
  """, custom_args, arg1, f"*{arg1}", f"{const}{typename}& {arg1}"
  #endif custom_parsing

  if optional:
    opt_typename = f"std::optional<{const}{typename} *>"
    return f"""
    {opt_typename} {arg1} = 
        check_user_optional_argument<{const}{typename}>(contract_memory, user_{arg1},
                                      sizeof({typename}));

    {check_arg_statement}
""", [f"user_{arg1}"], arg1, f"*{arg1}", f"{const}{typename}* {arg1}"
  # end optional arg

  # else fixed size
  return f"""
  {const}{typename} *{arg1} =
      check_user_argument<{const}{typename}>(contract_memory, user_{arg1},
                                      sizeof({typename}));

  {check_arg_statement}
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
  inner_return_type = "HostFnStatus<SyscallErrorCode>"

  if "syscall_return_type" in syscall:
    inner_return_type = syscall["syscall_return_type"]

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

  if len(user_args) > syscall_argcount:
    raise ValueError("too many syscall args")

  if len(user_args) < syscall_argcount:
    user_args += ["/*unused*/"] * (syscall_argcount - len(user_args))

  if syscall["needs_context"]:
    call_arg_strs = ["syscall_handler"] + call_arg_strs
    inner_arg_decl_strs = ["SystemCallHandler& syscall_handler"] + inner_arg_decl_strs
    #syscall_handler = "syscall_handler, "
    #syscall_handler_decl = "SystemCallHandler& syscall_handler, "

  inner_args = ", ".join(call_arg_strs)
  inner_arg_decls = ", ".join(inner_arg_decl_strs);
  outer= f"""std::pair<HostFnResult, GasAmount>
syscall_{name}_outer(SystemCallHandler& syscall_handler,
                     std::span<std::byte> contract_memory,
                     uint64_t {user_args[0]}, 
                     uint64_t {user_args[1]},
                     uint64_t {user_args[2]}, 
                     uint64_t {user_args[3]},
                     uint64_t {user_args[4]},
                     uint64_t {user_args[5]})
{{{inner_body}
  auto res = syscall_{name}_inner({inner_args});

  return {{convert_return_t(res.first), res.second}};
}}
"""
  inner = f"""std::pair<{inner_return_type}, GasAmount>
syscall_{name}_inner({syscall_handler_decl}{inner_arg_decls});

"""
  frame = f"""std::pair<{inner_return_type}, GasAmount>
syscall_{name}_inner({syscall_handler_decl}{inner_arg_decls})
{{
  throw std::runtime_error("unimplemented");
}}

"""
  return outer, inner, frame

def gen_outer(syscall):
  return gen_inner_outer(syscall)[0]

def gen_inner(syscall):
  return gen_inner_outer(syscall)[1]

def gen_frame(syscall):
  return gen_inner_outer(syscall)[2]


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

  syscall_uint64_arg_decls = []
  syscall_uint64_args = []
  for i in range(0, syscall_argcount + 1):
    syscall_uint64_arg_decls.append(
      f"uint64_t arg{i}")
    syscall_uint64_args.append(f"arg{i}")


  syscall_uint64_arg_decls_str = ", ".join(syscall_uint64_arg_decls)
  syscall_uint64_args_str = ", ".join(syscall_uint64_args)
  syscall_uint64_args_nozero_str = ", ".join(syscall_uint64_args[1:])

  return f"""#include "hogd/hogexec/runtime/syscall_entry.h"

#include <array>
#include <cassert>
#include <cstddef>
#include <cstring>
#include <optional>
#include <span>
#include <utility>
#include <type_traits>

#include "hogd/base/compiler.h"
#include "hogd/base/types.h"
#include "hogd/crypto/crypto.h"
#include "hogd/hogexec/runtime/gas_cost_table.h"
#include "hogd/hogexec/runtime/status.h"
#include "hogd/hogexec/runtime/syscall_argument_parsers.h"
#include "hogd/hogexec/runtime/syscall_inner.h"
#include "hogd/hogexec/runtime/wasmi_runtime.h"
#include "storage_object.hh"
#include "syscall.hh"
#include "types.hh"

namespace hogd {{

template<typename T>
std::pair<HostFnErrorCode, int32_t>
convert_return_t(T const& value) {{
  if constexpr (std::is_same_v<T, HostFnStatus<SyscallErrorCode>>) {{
    if (value) {{
      return {{HostFnErrorCode::kNoneOrRecoverable, static_cast<int32_t>(*value)}};
    }} else {{
      return {{value.error().code(), 0}};
    }}
  }} else {{
    static_assert(std::is_same_v<T, std::pair<HostFnErrorCode, int32_t>>);
    return value;
  }}
}}

{syscall_outers}

using SyscallFunc = std::pair<HostFnResult, uint64_t> (*)(
    SystemCallHandler&, std::span<std::byte>, {", ".join(["uint64_t"] * (syscall_argcount))});

{syscall_table}

// returns gas_consumed, syscall error>
std::pair<HostFnResult, uint64_t>
syscall_entry_inner(HostCallContext *call_context, {syscall_uint64_arg_decls_str})
{{
  if (arg0 >= syscall_table.size()) {{
    // invalid syscall
    return {{{{HostFnErrorCode::kNoneOrRecoverable, static_cast<int32_t>(SyscallErrorCode::kUnknownSyscall)}}, kUnknownSyscallCost}};
  }}

  static_assert(syscall_table.size() < INT32_MAX,
                "xdr enum overflow otherwise");

  auto [syscall_expect, syscall_fn_outer] = syscall_table[arg0];

  assert(syscall_expect == static_cast<SyscallNo>(arg0));

  std::span<std::byte> contract_memory = call_context->runtime->get_memory();

  assert(call_context -> syscall_handler);

  return syscall_fn_outer(*call_context->syscall_handler, contract_memory, {syscall_uint64_args_nozero_str});
}}

HostFnResult
syscall_entry(HostCallContext *call_context, {syscall_uint64_arg_decls_str})
{{
  auto [err, gas_consumed] =
      syscall_entry_inner(call_context, {syscall_uint64_args_str});

  if (!call_context->runtime->consume_gas(gas_consumed)) {{
    return {{HostFnErrorCode::kOutOfGas, 0}};
  }}

  static_assert(std::same_as<std::underlying_type_t<SyscallErrorCode>, int32_t>, "mismatch");

  return err;
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

def gen_rs_syscall_binding(syscall):
  name = syscall["name"]
  inputdecls = []
  syscallargs = []
  uninit_args = []
  if syscall["inputs"] is not None:
    for inp in syscall["inputs"]:

      optional = False
      if "optional" in inp and inp["optional"]:
        optional = True

      input_name = inp["name"]
      input_type = "[u8]"
      if "primitive_type" in inp and inp["primitive_type"]:
        input_type = inp["typename"]
        if "rust_typename" in inp:
          input_type = inp["rust_typename"]
        syscallargs.append(f"""{input_name} as u64""")
        inputdecls.append(f"""{input_name} : {input_type}""")
        uninit_args.append(f"""{input_name}""")
      elif "fixed_size" in inp and inp["fixed_size"]:

        input_type = inp["typename"]
        if "rust_typename" in inp:
          input_type = inp["rust_typename"]
        if optional:
          syscallargs.append(f"""match {input_name} {{ Some(x) => {{x.as_ptr() as u64 }}, None => u64::MAX }}""")
          inputdecls.append(f"""{input_name} : Option<&{input_type}>""")
          uninit_args.append(f"""{input_name}""")
        else:
          syscallargs.append(f"""{input_name}.as_ptr() as u64""")
          inputdecls.append(f"""{input_name} : &{input_type}""")
          uninit_args.append(f"""{input_name}""")

      elif "custom_parsing" in inp and inp["custom_parsing"]:
        # custom parsing, for now I just implement as a direct passthrough
        # and assume the sdk wrapper will handle.  Input types are just u64s
        for i in range(0, inp["custom_args_required"]):
          syscallargs.append(f"""{input_name}_{i}""")
          inputdecls.append(f"""{input_name}_{i} : u64""")
          uninit_args.append(f"""{input_name}_{i}""")
      else: # span argument
        if optional:
          syscallargs.append(f"""match {input_name} {{ Some(x) => {{x.as_ptr() as u64 }}, None => u64::MAX }}""")
          syscallargs.append(f"""match {input_name} {{ Some(x) => {{x.len() as u64 }}, None => 0 }}""")
          inputdecls.append(f"""{input_name} : Option<&{input_type}>""")
          uninit_args.append(f"""{input_name}""")
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
        if "rust_typename" in outp:
          output_type = outp["rust_typename"]
        syscallargs.append(f"""to_offset_mut({output_name}) as u64""")
        uninit_outputtype = output_type
        uninit_args.append(f"""{output_name}""")
      elif "fixed_size" in outp and outp["fixed_size"]:
        output_type = outp["typename"]
        if "rust_typename" in outp:
          output_type = outp["rust_typename"]
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

  if len(syscallargs) < syscall_argcount:
    syscallargs += ["0"] * (syscall_argcount - len(syscallargs))

  syscallargs_str = """,
            """.join(syscallargs)

  uninit_args_str = ", ".join(uninit_args)
  
  syscall_return_type = "Result<(), i32>"

  errdecl = "let err = "
  errhandle_str = """if err != SyscallErrorCode::kNone as i32 {
        return Err(err);
    }

    Ok(())"""

  if "returns" in syscall and not syscall["returns"]:
    syscall_return_type = "!"
    errhandle_str = "unsafe {unreachable_unchecked()}"
    errdecl = ""

  return f"""pub fn uninit_{name}({uninit_inputdecl_str}) -> {syscall_return_type} {{

    // syscall will initialize outputs, if error is kNone
    {errdecl}
        syscall_wrapper(SyscallNo::k{name},
            {syscallargs_str});

    {errhandle_str}
}}

pub fn raw_{name}({inputdecl_str}) -> {syscall_return_type} {{
    uninit_{name}({uninit_args_str})
}}

"""

def gen_rs_syscall_bindings(spec):
  syscall_str = ""
  for syscall in spec:
    syscall_str += gen_rs_syscall_binding(syscall)

  return f"""#![allow(non_snake_case)]
use rs_xdr::*;
use crate::syscall::{{syscall_wrapper, to_offset_mut}};
use core::mem::MaybeUninit;
use core::hint::unreachable_unchecked;

{syscall_str}

"""

def gen_syscall_frame(spec, args):
  req_calls = []
  for arg in args:
    for call in spec:
      if (call["number"] == arg):
        frame = gen_frame(call)
        req_calls.append(frame)

  joined = "".join(req_calls)

  return f"""#include "hogd/hogexec/runtime/syscall_inner.h"

#include <cstddef>
#include <span>
#include <utility>

#include "hogd/hogexec/runtime/status.h"
#include "hogd/hogexec/runtime/syscall_handler.h"
#include "syscall.hh"
#include "types.hh"

namespace hogd {{

{joined}
}}"""
  




parser = argparse.ArgumentParser(description="syscall generator")

parser.add_argument('--syscall_x', action='store_true', help="gen syscall.x")
parser.add_argument('--syscall_inner_h', action='store_true', help="gen syscall_inner.h")
parser.add_argument('--syscall_entry_cc', action='store_true', help="gen syscall_entry_cc")
parser.add_argument('--syscall_yaml', type=str, required = True, help = "input yaml")
parser.add_argument('--syscall_rs', action='store_true', help = 'gen rust syscall bindings')
parser.add_argument('--output_dir', type=str, default = ".", help = "output folder")
parser.add_argument('--syscall_frame', nargs='+', type=int, help = "syscall nos")

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

if (args.syscall_rs):
  filename = args.output_dir + "/syscall_bindings.rs"
  with open(filename, "w") as f:
    f.write(gen_rs_syscall_bindings(spec))

if (args.syscall_frame is not None and len(args.syscall_frame) > 0):
  print(gen_syscall_frame(spec, args.syscall_frame))

