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
""", [f"user_{arg1}", f"user_{arg2}"], arg1, f"std::span<{const}std::byte> {arg1}"
  # fixed size
  typename = inp["typename"]
  arg1 = f"{in_out}_{arg_name}"
  return f"""
  {const}{typename} *{arg1} =
      check_user_argument<{const}{typename}>(contract_memory, user_{arg1},
                                      sizeof({typename}));

  if (!{arg1}) {{
    return {{SyscallErrorCode::kFault, kGenericBadSyscallCost}};
  }}
""", [f"user_{arg1}"], arg1, f"{const}{typename}& {arg1}"


def gen_inner_outer(syscall):
  name = syscall["name"]
  arg1 = "/*unused*/"
  arg2 = "/*unused*/"
  arg3 = "/*unused*/"
  arg4 = "/*unused*/"
  inner_body = ""
  call_arg_strs = []
  user_args = []
  inner_arg_decl_strs = []
  syscall_handler = ""
  syscall_handler_decl = ""
  if syscall["inputs"] is not None:
    for arg in syscall["inputs"]:
      validate_str, local_user_args, local_arg, inner_arg_decl = gen_validate_arg(arg, is_input = True)
      inner_body += validate_str
      user_args += local_user_args
      call_arg_strs.append(f"*{local_arg}")
      inner_arg_decl_strs.append(inner_arg_decl)

  if syscall["outputs"] is not None:
    for arg in syscall["outputs"]:
      validate_str, local_user_args, local_arg, inner_arg_decl = gen_validate_arg(arg, is_input = False)
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

  if syscall["needs_context"]:
    syscall_handler = "syscall_handler, "
    syscall_handler_decl = "SystemCallHandler& syscall_handler, "


  inner_args = ", ".join(call_arg_strs)
  inner_arg_decls = ", ".join(inner_arg_decl_strs);
  outer= f"""std::pair<HostFnStatus<SyscallErrorCode>, uint64_t>
syscall_{name}_outer(SystemCallHandler& syscall_handler,
                     std::span<std::byte> contract_memory,
                     uint64_t {arg1}, 
                     uint64_t {arg2},
                     uint64_t {arg3}, 
                     uint64_t {arg4})
{{{inner_body}
  return syscall_{name}_inner({syscall_handler}{inner_args});
}}
"""
  inner = f"""std::pair<HostFnStatus<SyscallErrorCode>, uint64_t>
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

{syscall_outers}

using SyscallFunc = std::pair<HostFnStatus<SyscallErrorCode>, uint64_t> (*)(
    SystemCallHandler&, std::span<std::byte>, uint64_t, uint64_t, uint64_t, uint64_t);

{syscall_table}

// returns gas_consumed, syscall error>
std::pair<HostFnStatus<SyscallErrorCode>, uint64_t>
syscall_entry_inner(HostCallContext *call_context, uint64_t arg0, uint64_t arg1,
                    uint64_t arg2, uint64_t arg3, uint64_t arg4)
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

  return syscall_fn_outer(*call_context->syscall_handler, contract_memory, arg1, arg2, arg3, arg4);
}}

HostFnStatus<uint64_t>
syscall_entry(HostCallContext *call_context, uint64_t arg0, uint64_t arg1,
              uint64_t arg2, uint64_t arg3, uint64_t arg4)
{{
  auto [err, gas_consumed] =
      syscall_entry_inner(call_context, arg0, arg1, arg2, arg3, arg4);

  if (!call_context->runtime->consume_gas(gas_consumed)) {{
    return HostFnError{{HostFnErrorCode::kOutOfGas}};
  }}
  return err.and_then([](SyscallErrorCode err) -> HostFnStatus<uint64_t> {{ return static_cast<uint64_t>(err); }});
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
// clang-format off
%#include "types.hh"

namespace hogd {{
// clang-format on

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


parser = argparse.ArgumentParser(description="syscall generator")

# Add arguments
parser.add_argument('--syscall_x', action='store_true', help="gen syscall.x")
parser.add_argument('--syscall_inner_h', action='store_true', help="gen syscall_inner.h")
parser.add_argument('--syscall_entry_cc', action='store_true', help="gen syscall_entry_cc")
parser.add_argument('--output_dir', type=str, required = True, help = "output folder")
parser.add_argument('--syscall_yaml', type=str, required = True, help = "input yaml")
parser.add_argument('--status_cc', action='store_true', help="gen status.cc")
parser.add_argument('--template_filename', type=str, help = "template filename")

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


