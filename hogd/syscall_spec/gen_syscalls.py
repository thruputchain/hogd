#!/bin/python3

import yaml
from yaml import CLoader as Loader

import argparse
import os


def load_cpp_to_rust_map(spec: dict) -> dict[str, str]:
    cpp_to_rust_typenames: dict[str, str] = {}
    for type_mapping in spec["type_mappings"]:
        cpp_to_rust_typenames[type_mapping["base_name"]] = (
            type_mapping["rust_name"]
        )
    return cpp_to_rust_typenames


def load_cpp_to_xdr_map(spec: dict) -> dict[str, str]:
    cpp_to_xdr_typenames = {}
    for type_mapping in spec["type_mappings"]:
        cpp_to_xdr_typenames[type_mapping["base_name"]] = (
            type_mapping["xdr_name"]
        )
    return cpp_to_xdr_typenames


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
  read_only: yes/no (default: no)

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

read_only:
  type: boolean
  required: no
    default: no
  description: Defines whether or not the syscall is read-only.
  If false, the syscall will fail if the current invocation is read-only.

inputs and outputs are lists of parameters, of the following yaml type. The
inputs and outputs are lists of parameters. The logic is largely the same,
except that inputs are const and outputs are not, and variables are named
input_{name} or output_{name} appropriately.

name:
  type: string
  required: yes
  description: Name of the argument (either input_{name} or output_{name}).

typename: string
  required: no
    default: none
  description: The type of the supplied argument.  Must be a trivially copyable
  type.  `typename` must be present in both the syscall code (e.g., an XDR
  type, or a basic type) and be present in contract code.  For types that have
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


def gen_validate_arg(inp: dict, *, is_input: bool) -> tuple[int, str]:
    arg_name = inp["name"]
    const = ""
    in_out = "output"
    if is_input:
        const = "const "
        in_out = "input"

    optional = False
    if "optional" in inp and inp["optional"]:
        optional = True

    # put optional_ on front of arg name, for readability
    arg_name = f"{in_out}_{arg_name}"
    if optional:
        arg_name = f"optional_{arg_name}"

    if "span_argument" in inp and inp["span_argument"]:
        if optional:
            return 2, f"std::optional<std::span<{const}std::byte>> {arg_name}"
        else:
            return 2, f"std::span<{const}std::byte> {arg_name}"
    # endif span_argument

    typename = inp["typename"]

    if "primitive_type" in inp and inp["primitive_type"] and is_input:
        if optional:
            raise Exception("optional primitive_type is unimplemented")
        return 1, f"{typename} {arg_name}"

    if optional:
        return 1, f"{const}{typename}* {arg_name}"
    # end optional arg

    # else fixed size
    if "syscall_pass_by_value" in inp and inp["syscall_pass_by_value"]:
        if not is_input:
            raise Exception("Pass by value only valid for inputs")
        return 1, f"{typename} {arg_name}"
    return 1, f"{const}{typename}& {arg_name}"


def gen_inner_outer(
    syscall: dict,
    syscall_input_count: int
) -> tuple[str, str]:
    name = syscall["name"]
    user_arg_count = 0
    inner_arg_decl_strs = []
    syscall_handler_decl = ""
    inner_return_type = "HostFnStatus<SyscallErrorCode>"

    if syscall["inputs"] is not None:
        for arg in syscall["inputs"]:
            local_user_arg_count, inner_arg_decl = (
                gen_validate_arg(arg, is_input=True)
            )
            user_arg_count += local_user_arg_count
            inner_arg_decl_strs.append(inner_arg_decl)

    if syscall["outputs"] is not None:
        for arg in syscall["outputs"]:
            local_user_arg_count, inner_arg_decl = (
                gen_validate_arg(arg, is_input=False)
            )
            user_arg_count += local_user_arg_count
            inner_arg_decl_strs.append(inner_arg_decl)

    if user_arg_count > syscall_input_count:
        raise ValueError("too many syscall args")

    if "needs_gas" in syscall and syscall["needs_gas"]:
        inner_arg_decl_strs = [
            "WrappedGasAmount remaining_gas"
        ] + inner_arg_decl_strs

    # needs context defaults to True
    needs_context = True
    if "needs_context" in syscall and not syscall["needs_context"]:
        needs_context = False
    if needs_context:
        inner_arg_decl_strs = [
            "SystemCallHandler& syscall_handler"
        ] + inner_arg_decl_strs

    inner_arg_decls = ", ".join(inner_arg_decl_strs)

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


def gen_inner(syscall: dict, syscall_input_count: int) -> str:
    return gen_inner_outer(syscall, syscall_input_count)[0]


def gen_frame(syscall: dict, syscall_input_count: int) -> str:
    return gen_inner_outer(syscall, syscall_input_count)[1]


def gen_syscall_meta(syscall: dict) -> str:
    name = syscall["name"]
    read_only = "false"
    if "read_only" in syscall and syscall["read_only"]:
        read_only = "true"
    return f"""template<>
struct SyscallMeta<SyscallNo::k{name}> {{
    constexpr static bool kIsReadOnlySyscall = {read_only};
    constexpr static auto kInnerFunc = &syscall_{name}_inner;
}};
"""


def gen_syscall_meta_h(spec_full: dict) -> str:
    spec = spec_full["syscalls"]
    syscall_meta = ""
    for syscall in spec:
        syscall_meta += gen_syscall_meta(syscall)
    return f"""// -*-C++-*-
#pragma once

#include "syscall.hh"
#include "hogd/hogexec/runtime/syscall_inner.h"

namespace hogd {{

template<SyscallNo Syscall>
struct SyscallMeta;

{syscall_meta}

}}
"""


def gen_syscall_inner_h(spec_full: dict) -> str:
    inner_decls = ""
    syscall_argcount = spec_full["syscall_input_count"]
    spec = spec_full["syscalls"]

    for syscall in spec:
        inner_decls += gen_inner(syscall, syscall_argcount)

    return f"""// -*-C++-*-

// *** Generated Code  ***
// *** Do not check in ***

#pragma once

#include <cstddef>
#include <span>
#include <utility>

#include "hogd/base/types.h"
#include "hogd/hogexec/runtime/status.h"
#include "hogd/hogexec/runtime/syscall_types.h"
#include "syscall.hh"
#include "transaction.hh"
#include "types.hh"

namespace hogd {{

class SystemCallHandler;
class Runtime;

// NOLINTBEGIN(readability-identifier-naming)
{inner_decls}
// NOLINTEND(readability-identifier-naming)

}}// namespace hogd
"""


# ===== generate syscall-related x files =====

# One argument within a SyscallArgs structure.
def gen_syscall_io_arg_decl(arg: dict) -> str:
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


def gen_syscall_input_arg_xdr(syscall: dict) -> tuple[str, str]:
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


def gen_syscall_output_arg_xdr(syscall: dict) -> tuple[str, str]:
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


def gen_syscall_arg_xdr(spec: dict) -> str:
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


def gen_syscall_x(spec_full: dict) -> str:
    spec = spec_full["syscalls"]
    syscalls: dict[int, str] = {}
    errors: dict[int, dict] = {}
    for syscall in spec:
        syscalls[syscall["number"]] = syscall["name"]

    enumdecls = ""

    for i in range(0, len(syscalls)):
        name = syscalls[i]
        enumdecls += f"    k{name} = {i},\n"

    error_spec = spec_full["errors"]

    for error in error_spec:
        errors[error["number"]] = error

    error_enumdecls = ""

    for num in errors.keys():
        name = errors[num]["name"]
        docstring = errors[num]["doc"]
        error_enumdecls += f"\n    // {docstring}\n    k{name} = {num},\n"

    return f"""// clang-format off
%// *** Generated Code  ***
%// *** Do not check in ***

%#include "types.hh"

namespace hogd {{
// clang-format on

enum SyscallNo {{
{enumdecls}}};

const kSyscallCount = {len(syscalls)};

enum SyscallErrorCode {{{error_enumdecls}}};

}} // namespace hogd
"""


# ===== end generate syscall-related x files =====

def gen_syscall_frame(spec: list[dict], args: list[int]) -> str:
    req_calls = []
    for arg in args:
        for call in spec:
            if call["number"] == arg:
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
    required=True,
    help="input yaml")
parser.add_argument(
    '--output_dir',
    type=str,
    default=".",
    help="output folder")
parser.add_argument(
    '--syscall_frame',
    nargs='+',
    type=int,
    help="syscall nos")
parser.add_argument(
    '--syscall_meta_h',
    action='store_true',
    help="gen syscall_meta.h")

args = parser.parse_args()

spec_full = yaml.load(open(args.syscall_yaml, 'r').read(), Loader=Loader)
spec = spec_full["syscalls"]

cpp_to_rust_typenames = load_cpp_to_rust_map(spec_full)
cpp_to_xdr_typenames = load_cpp_to_xdr_map(spec_full)

os.makedirs(args.output_dir, exist_ok=True)

if args.syscall_x:
    filename = args.output_dir + "/syscall.x"
    with open(filename, "w") as f:
        f.write(gen_syscall_x(spec_full))

if args.syscall_args_x:
    filename = args.output_dir + "/syscall_args.x"
    with open(filename, "w") as f:
        f.write(gen_syscall_arg_xdr(spec_full))

if args.syscall_inner_h:
    filename = args.output_dir + "/syscall_inner.h"
    with open(filename, "w") as f:
        f.write(gen_syscall_inner_h(spec_full))

if args.syscall_meta_h:
    filename = args.output_dir + "/syscall_meta.h"
    with open(filename, "w") as f:
        f.write(gen_syscall_meta_h(spec_full))

if args.syscall_frame is not None and len(args.syscall_frame) > 0:
    print(gen_syscall_frame(spec, args.syscall_frame))
