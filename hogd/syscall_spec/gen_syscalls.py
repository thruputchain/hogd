#!/bin/python3

import yaml

from yaml import CLoader as Loader, CDumper as Dumper

import argparse
import os

# Map from c++ to xdr typenames
cpp_to_xdr_typenames = {
  "int32_t" : "int",
  "uint32_t" : "unsigned int",
  "int64_t" : "hyper",
  "uint64_t" : "unsigned hyper"
}

# map from c++ to rust typenames
cpp_to_rust_typenames = {
  "int32_t" : "i32",
  "uint32_t" : "u32",
  "int64_t" : "i64",
  "uint64_t" : "u64"
}

"""
Each syscall generates a
syscall_{callname}_inner(...); declaration.
The interface is typed, and not responsible for
e.g. validating OOB accesses for the input arguments.

Each syscall is specified in the following (example) format:
- name: "XYZ"
  needs_context: yes/no
  inputs:
    - list of inputs
  outputs:
    - list of outputs
  returns: yes/no (default: yes)
  number: 0

Syscall yaml specification is as follows.

name:
  type: string
  required: yes
  description: The name of the syscall.

needs_context:
  type: boolean (yes/no)
  required: no
    default: true
  description: Defines whether or not a SyscallHandler& reference is passed
  to the syscall.  Most syscalls will have this as a yes; there are
  a few (i.e. SHA256) where this is not required.

needs_gas:
  type: boolean
  required: no
  description: Whether or not the amount of currently available gas
  should be passed as input to the syscall.
  default: no
  
returns:
  type: boolean
  required: no
    default: no
  description: Defines whether or not the function returns to the caller
  or terminates the caller runtime instance.

inputs and outputs are lists of parameters, of the following yaml type.
The inputs and outputs are lists of parameters.
The logic is largely the same, except that inputs are const and outputs are not,
and variables are named input_{name} or output_{name} appropriately.

name:
  type: string
  required: yes
  description: Name of the argument (either input_{name} or output_{name}).

typename: string
  required: no
    default: none
  description: The type of the supplied argument.  Must be a trivially copyable
  type.  `typename` must be present in both the syscall code (e.g., an XDR type,
  or a basic type) and be present in contract code.  For types that have 
  different names, the optional `rust_typename` parameter may be set.

primitive_type: bool
  required: no
    default: no
  description: If the trivially copyable type is a primitive type that can
  fit within a uint64_t, then this argument will be passed by value
  as one of the arguments to the syscall, instead of as an offset into contract 
  memory.

  Only available for input arguments.

syscall_pass_by_value: bool
  required: no
    default: no
  description: Pass the input argument to the syscall by value, instead of by
  constant reference (input arguments only).

rust_typename: string
  required: no
    default: the value set in typename
  description: Used when rust names a type differently from the C++ code
  in the syscall.

optional: bool
  required: no
    default: no

  description: This syscall argument is optional.  
  Passed to syscall_{name}_inner as a pointer, instead of a reference.

  Not available with primitive_type = yes.

span_argument: bool
  required: no
    default: no
  description: The argument is a bytestring (a span) into contract memory.
  Passed into c++ as a std::span<{const}std::byte>.

  Mutually exclusive with typename or primitive_type.

custom_parsing: bool
  required: no
    default: no
  description: Some input argument types require parsing in ways that don't 
  quite fit into one of the above options. These we support with a custom 
  parsing option.

custom_args_required: int
  required: only if custom_parsing is true
  description: The number of arguments passed to the custom parsing handler.
"""

"""
Generate the declaration details for one syscall input or output

Return format:
local_user_arg_count, inner_arg_decl

local_user_args_count is the number of uint64_t syscall inputs
consumed by this argument.  Used only in internal assertions.

inner_arg_decl is the parameter of the inner syscall declaration corresponding 
to this argument.
"""
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
    arg_name = f"{in_out}_{arg_name}"
    if optional:
      return 2, f"std::optional<std::span<{const}std::byte>> {arg_name}"
    else:
      return 2, f"std::span<{const}std::byte> {arg_name}"
  # endif span_argument

  typename = inp["typename"]
  arg_name = f"{in_out}_{arg_name}"

  # put optional_ on front of arg name, for readability
  if optional:
    arg_name = f"optional_{arg_name}"
  
  if "primitive_type" in inp and inp["primitive_type"] and is_input:
    if optional:
      raise Exception("optional primitive_type is unimplemented");
    return 1, f"{typename} {arg_name}"

  if "custom_parsing" in inp and inp["custom_parsing"] and is_input:
    if optional:
      raise Exception("optional parsing is a strict subset of custom parsing--don't use both")
    num_args = inp["custom_args_required"]

    return num_args, f"{const}{typename}& {arg_name}"
  #endif custom_parsing

  if optional:
    return 1, f"{const}{typename}* {arg_name}"
  # end optional arg

  # else fixed size
  if "syscall_pass_by_value" in inp and inp["syscall_pass_by_value"]:
    if not is_input:
      raise Exception("Pass by value only valid for inputs")
    return 1, f"{typename} {arg_name}"
  return 1, f"{const}{typename}& {arg_name}"

def gen_inner_outer(syscall, syscall_input_count):
  name = syscall["name"]

  user_arg_count = 0
  inner_arg_decl_strs = []

  syscall_handler_decl = ""
  inner_return_type = "HostFnStatus<SyscallErrorCode>"

  if "syscall_return_type" in syscall:
    inner_return_type = syscall["syscall_return_type"]

  if syscall["inputs"] is not None:
    for arg in syscall["inputs"]:
      local_user_arg_count, inner_arg_decl = gen_validate_arg(arg, is_input = True)
      user_arg_count += local_user_arg_count
      inner_arg_decl_strs.append(inner_arg_decl)

  if syscall["outputs"] is not None:
    for arg in syscall["outputs"]:
      local_user_arg_count, inner_arg_decl  = gen_validate_arg(arg, is_input = False)
      user_arg_count += local_user_arg_count
      inner_arg_decl_strs.append(inner_arg_decl)

  if user_arg_count > syscall_input_count:
    raise ValueError("too many syscall args")
  
  if "needs_gas" in syscall and syscall["needs_gas"]:
    inner_arg_decl_strs = ["WrappedGasAmount remaining_gas"] + inner_arg_decl_strs
  
  #needs context defaults to True
  needs_context = True
  if "needs_context" in syscall and not syscall["needs_context"]:
    needs_context = False
  if needs_context:
    inner_arg_decl_strs = ["SystemCallHandler& syscall_handler"] + inner_arg_decl_strs

  inner_arg_decls = ", ".join(inner_arg_decl_strs);

  inner = f"""std::pair<{inner_return_type}, GasAmount>
syscall_{name}_inner({syscall_handler_decl}{inner_arg_decls});

"""
  frame = f"""std::pair<{inner_return_type}, GasAmount>
syscall_{name}_inner({syscall_handler_decl}{inner_arg_decls})
{{
  throw std::runtime_error("unimplemented");
}}

"""
  return inner, frame

def gen_inner(syscall, syscall_input_count):
  return gen_inner_outer(syscall, syscall_input_count)[0]

def gen_frame(syscall, syscall_input_count):
  return gen_inner_outer(syscall, syscall_input_count)[1]


def gen_syscall_table_entry(syscall):
  name = syscall["name"]
  return f"""    std::pair{{SyscallNo::k{name}, &wrap_outer<SyscallNo::k{name}, &syscall_{name}_inner>}},
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
  table = f"""constexpr static std::array<std::pair<SyscallNo, SyscallFunc>, {max_seen}> kSyscallTable = {{
"""
  for i in range(0, max_seen):
    table += gen_syscall_table_entry(syscall_map[i])
  table += "};"
  return table

def gen_syscall_inner_h(spec_full):
  inner_decls=""
  syscall_argcount = spec_full["syscall_input_count"]
  spec = spec_full["syscalls"]

  for syscall in spec:
    inner_decls += gen_inner(syscall, syscall_argcount)

    syscall_table = gen_syscall_table(spec)

  return f"""// -*-C++-*-

// *** Generated Code  ***
// *** Do not check in ***

#pragma once

#include <array>
#include <utility>

#include "hogd/base/types.h"
#include "hogd/hogexec/runtime/status.h"
#include "hogd/hogexec/runtime/syscall_entry_outer.h"
#include "syscall.hh"
#include "types.hh"

namespace hogd {{

class SystemCallHandler;
class Runtime;

{inner_decls}

using SyscallFunc = std::pair<HostFnStatus<SyscallErrorCode>, uint64_t> (*)(
    SystemCallHandler&, Runtime&, {", ".join(["uint64_t"] * (syscall_argcount))});

{syscall_table}

}}// namespace hogd
"""

# ===== generate syscall-related x files =====

# One argument within a SyscallArgs structure.
def gen_syscall_io_arg_decl(arg):
  end_decorator = ""
  argtype = ""
  if "xdr_typename" in arg:
    argtype = arg["xdr_typename"]
  elif "typename" in arg and arg["typename"] in cpp_to_xdr_typenames.keys():
    argtype = cpp_to_xdr_typenames[arg["typename"]]
  elif "span_argument" in arg and arg["span_argument"]:
    argtype = "opaque"
    end_decorator = "<>"
  else:
    argtype = arg["typename"]

  pointerdecl = ""
  if "optional" in arg and arg["optional"]:
    pointerdecl = "*"
  argname = arg["name"]
  return f"  {argtype} {pointerdecl}{argname}{end_decorator};"

def gen_syscall_input_arg_xdr(syscall):
  name = syscall["name"]
  decls = []

  if syscall["inputs"] is not None:
    for arg in syscall["inputs"]:
      decl = gen_syscall_io_arg_decl(arg)
      decls = decls + [decl]
  if len(decls) == 0:
    decls = ["  void;"]
  decls_joined = "\n".join(decls)

  return f"""struct {name}_input {{
{decls_joined}
}};

""", f"""  case k{name}:
    {name}_input {name};
"""

def gen_syscall_output_arg_xdr(syscall):
  name = syscall["name"]
  decls = []

  if syscall["outputs"] is not None:
    for arg in syscall["outputs"]:
      decl = gen_syscall_io_arg_decl(arg)
      decls = decls + [decl]
  if len(decls) == 0:
    decls = ["  void;"]
  decls_joined = "\n".join(decls)

  return f"""struct {name}_output {{
{decls_joined}
}};

""", f"""  case k{name}:
    {name}_output {name};
"""

def gen_syscall_arg_xdr(spec):
  structs = ""
  decls_in = ""
  decls_out = ""
  for call in spec["syscalls"]:
    struct, decl = gen_syscall_input_arg_xdr(call)
    structs += struct
    decls_in += decl
    struct, decl = gen_syscall_output_arg_xdr(call)
    structs += struct
    decls_out += decl

  return f"""// clang-format off
%#include "types.hh"
%#include "syscall.hh"
%#include "transaction.hh"

namespace hogd {{
  // clang-format on
{structs}

union SyscallInput switch(SyscallNo syscall) {{
{decls_in}
}};

union SyscallOutput switch(SyscallNo syscall) {{
{decls_out}
}};

}}

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

  for num in errors.keys():
    name = errors[num]["name"]
    docstring=errors[num]["doc"]
    error_enumdecls += f"\n    // {docstring}\n    k{name} = {num},\n"


  return f"""// clang-format off
%// *** Generated Code  ***
%// *** Do not check in ***

%#include "types.hh"

namespace hogd {{
// clang-format on

enum SyscallNo {{
{enumdecls}}};

enum SyscallErrorCode {{{error_enumdecls}}};

}} // namespace hogd
"""

# ===== end generate syscall-related x files =====

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

# ===== Mock syscall entry =====
def gen_mock_switch_case(syscall, syscall_argcount):
  name = syscall["name"]
  args = []
  for i in range(0, syscall_argcount):
    args.append(f"arg{i + 1}")
  args_str = ", ".join(args)
  return f"""  case SyscallNo::k{name}:
  return mock_syscall_parser<SyscallNo::k{name}, {name}_input, {name}_output,
                               &syscall_{name}_inner>({args_str});
"""

def gen_mock_syscall_entry_cc(spec_full):
  spec = spec_full["syscalls"]
  syscall_argcount = spec_full["syscall_input_count"]
  switch_body = ""
  for call in spec:
    switch_body += gen_mock_switch_case(call, syscall_argcount)

  args = []
  for i in range(0, syscall_argcount):
    args.append(f"uint64_t arg{i + 1}")
  args_str = ", ".join(args)
  
  return f"""#include "hogd/test_ffi/mock_syscall_entry.h"

#include <span>

#include "hogd/base/types.h"
#include "syscall.hh"
#include "syscall_args.hh"
#include "hogd/test_ffi/syscall_handler_wrapper.h"
#include "hogd/hogexec/runtime/syscall_inner.h"

namespace hogd {{

ForeignHostFnResult
mock_syscall_entrypoint(uint64_t syscall,
                        {args_str}) noexcept
{{
  auto syscall_no = static_cast<SyscallNo>(syscall);

  switch (syscall_no) {{
{switch_body}
  default:
    return recoverable_error(SyscallErrorCode::kNoSyscall);
  }}
}}

}}
"""
# ===== end mock syscall entry =====

parser = argparse.ArgumentParser(description="syscall generator")

parser.add_argument(
  '--syscall_x', 
  action='store_true', 
  help="gen syscall.x")
parser.add_argument(
  '--syscall_args_x', 
  action='store_true', 
  help="gen syscall_args.x")
parser.add_argument(
  '--syscall_inner_h', 
  action='store_true', 
  help="gen syscall_inner.h")
parser.add_argument(
  '--syscall_yaml', 
  type=str, 
  required = True, 
  help = "input yaml")
parser.add_argument(
  '--syscall_rs', 
  action='store_true', 
  help = 'gen rust syscall bindings')
parser.add_argument(
  '--output_dir', 
  type=str, 
  default = ".", 
  help = "output folder")
parser.add_argument(
  '--syscall_frame', 
  nargs='+', 
  type=int, 
  help = "syscall nos")
parser.add_argument(
  '--mock_syscall_entry_cc', 
  action='store_true', 
  help="gen mock_syscall_entry_cc")

args = parser.parse_args()

spec_full = yaml.load(open(args.syscall_yaml, 'r').read(), Loader=Loader)
spec = spec_full["syscalls"]

os.makedirs(args.output_dir, exist_ok=True)

if (args.syscall_x):
  filename = args.output_dir + "/syscall.x"
  with open(filename, "w") as f:
    f.write(gen_syscall_x(spec_full))

if (args.syscall_args_x):
  filename = args.output_dir + "/syscall_args.x"
  with open(filename, "w") as f:
    f.write(gen_syscall_arg_xdr(spec_full))

if (args.syscall_inner_h):
  filename = args.output_dir + "/syscall_inner.h"
  with open(filename, "w") as f:
    f.write(gen_syscall_inner_h(spec_full))

if (args.syscall_frame is not None and len(args.syscall_frame) > 0):
  print(gen_syscall_frame(spec, args.syscall_frame))

if (args.mock_syscall_entry_cc):
  filename = args.output_dir + "/mock_syscall_entry.cc"
  with open(filename, "w") as f:
    f.write(gen_mock_syscall_entry_cc(spec_full))

