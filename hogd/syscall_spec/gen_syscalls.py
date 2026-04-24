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


def load_known_primitive_types(spec: dict) -> list[str]:
    return [
        type_mapping["base_name"]
        for type_mapping in spec["type_mappings"]
        if ("primitive_type" in type_mapping and
            type_mapping["primitive_type"])
    ]


def validate_and_extract_error_codes(syscall: dict) -> list[str]:
    """
    Extract and validate error codes from a syscall specification.
    Returns a list of valid error code names.
    Raises ValueError if an invalid error code is found.
    """
    errors = syscall.get("errors", None)
    if errors is None:
        return []

    error_codes = []
    for error in errors:
        code = error.get("code", None)
        if code is None:
            raise ValueError(
                f"Syscall {syscall['name']}: error entry missing 'code' field"
            )
        if code in error_codes:
            raise ValueError(
                f"Syscall {syscall['name']}: duplicate error code '{code}'"
            )
        error_codes.append(code)

    return error_codes


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

    primitive_type = False
    if (is_input and
        (("primitive_type" in inp and inp["primitive_type"]) or
         ("typename" in inp and inp["typename"] in primitive_types))):
        primitive_type = True

    optional = False
    if "optional" in inp and inp["optional"]:
        primitive_type = False
        optional = True

    # put optional_ on front of arg name, for readability
    arg_name = f"{in_out}_{arg_name}"
    if optional:
        arg_name = f"optional_{arg_name}"

    if "span_argument" in inp and inp["span_argument"]:
        if "span_type" in inp and inp["span_type"]:
            span_type = inp["span_type"]
        else:
            span_type = "std::byte"
        if optional:
            return 2, f"std::optional<std::span<{const}{span_type}>> {arg_name}"
        else:
            return 2, f"std::span<{const}{span_type}> {arg_name}"
    # endif span_argument

    typename = inp["typename"]

    if primitive_type:
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

    if "inputs" in syscall and syscall["inputs"] is not None:
        for arg in syscall["inputs"]:
            local_user_arg_count, inner_arg_decl = (
                gen_validate_arg(arg, is_input=True)
            )
            user_arg_count += local_user_arg_count
            inner_arg_decl_strs.append(inner_arg_decl)

    if "outputs" in syscall and syscall["outputs"] is not None:
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

    inner = f"""std::pair<{inner_return_type}, WrappedGasAmount>
syscall_{name}_inner({syscall_handler_decl}{inner_arg_decls}) noexcept;

"""
    frame = f"""std::pair<{inner_return_type}, WrappedGasAmount>
syscall_{name}_inner({syscall_handler_decl}{inner_arg_decls}) noexcept
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

    syscall_input_names = []
    syscall_output_names = []
    if "inputs" in syscall and syscall["inputs"] is not None:
        for arg in syscall["inputs"]:
            syscall_input_names.append("\"" + arg["name"] + "\"")
    if "outputs" in syscall and syscall["outputs"] is not None:
        for arg in syscall["outputs"]:
            syscall_output_names.append("\"" + arg["name"] + "\"")
    can_return = "true"
    if "returns" in syscall and not syscall["returns"]:
        can_return = "false"

    # Extract and validate error codes
    error_codes = validate_and_extract_error_codes(syscall)
    error_codes_cpp = [f"SyscallErrorCode::{code}" for code in error_codes]

    return f"""template<>
struct SyscallMeta<SyscallNo::k{name}> {{
    constexpr static bool kIsReadOnlySyscall = {read_only};
    constexpr static auto kInnerFunc = &syscall_{name}_inner;
    constexpr static SyscallNo kSyscallNo = SyscallNo::k{name};
    constexpr static bool kCanReturn = {can_return};

    constexpr static std::array<std::string_view, {len(syscall_input_names)}>
        kInputNames = {{
        {", ".join(syscall_input_names)}
    }};
    constexpr static std::array<std::string_view, {len(syscall_output_names)}>
        kOutputNames = {{
        {", ".join(syscall_output_names)}
    }};
    constexpr static std::array<SyscallErrorCode, {len(error_codes_cpp)}>
        kAllowedErrors = {{
        {",\n        ".join(error_codes_cpp)}
    }};
}};
static_assert(is_syscall_meta<SyscallMeta<SyscallNo::k{name}>>);

"""


def gen_syscall_meta_h(spec_full: dict) -> str:
    spec = spec_full["syscalls"]
    syscall_meta = ""
    for syscall in spec:
        syscall_meta += gen_syscall_meta(syscall)
    return f"""// -*-C++-*-
#pragma once

#include <string_view>
#include <array>

#include "hogd/hogexec/runtime/syscall_inner.h"
#include "hogd/hogexec/runtime/syscall_meta_concept.h"
#include "syscall.hh"
#include "syscall_error.hh"

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

#include "event.hh"
#include "hogd/base/types.h"
#include "hogd/hogexec/common/status.h"
#include "hogd/hogexec/runtime/syscall_types.h"
#include "hogd/rust/contract_types/bindings.h"
#include "invoke_flag.hh"
#include "storage_item.hh"
#include "syscall_error.hh"
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


def gen_syscall_x(spec_full: dict) -> str:
    spec = spec_full["syscalls"]
    syscalls: dict[int, str] = {}
    for syscall in spec:
        syscalls[syscall["number"]] = syscall["name"]

    enumdecls = ""

    for i in range(0, len(syscalls)):
        name = syscalls[i]
        enumdecls += f"    k{name} = {i},\n"

    return f"""// clang-format off
%// *** Generated Code  ***
%// *** Do not check in ***

%#include "types.hh"

namespace hogd {{
// clang-format on

enum SyscallNo {{
{enumdecls}}};

const kSyscallCount = {len(syscalls)};

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

#include "hogd/hogexec/common/status.h"
#include "hogd/hogexec/runtime/syscall_handler.h"
#include "syscall.hh"
#include "types.hh"

namespace hogd {{

{joined}
}}"""


def validate_syscall_numbers_in_order(spec_full: dict) -> None:
    syscalls_list = spec_full["syscalls"]
    numbers = [syscall["number"] for syscall in syscalls_list]
    expected_numbers = list(range(len(numbers)))
    if numbers != expected_numbers:
        names = [syscall["name"] for syscall in syscalls_list]
        raise ValueError(
            "Invalid syscall numbering/order. Syscalls must be defined in "
            "strictly increasing numerical order starting at 0 with no gaps."
            f"Expected: {expected_numbers}; Got: {numbers}; Names: {names}"
        )


parser = argparse.ArgumentParser(description="syscall generator")

parser.add_argument(
    '--syscall_x',
    action='store_true',
    help="gen syscall.x")
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

# Validate syscall numbering and order early to provide a clear error message
validate_syscall_numbers_in_order(spec_full)

cpp_to_rust_typenames = load_cpp_to_rust_map(spec_full)
primitive_types = load_known_primitive_types(spec_full)

os.makedirs(args.output_dir, exist_ok=True)

if args.syscall_x:
    filename = args.output_dir + "/syscall.x"
    with open(filename, "w") as f:
        f.write(gen_syscall_x(spec_full))

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
