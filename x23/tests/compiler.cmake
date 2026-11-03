cmake_minimum_required(VERSION 3.28)
file(MAKE_DIRECTORY "${TEST_DIR}")

function(run_compiler expected)
  execute_process(COMMAND "${X23C}" ${ARGN}
    WORKING_DIRECTORY "${TEST_DIR}"
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
  if(NOT result STREQUAL "${expected}")
    message(FATAL_ERROR "x23c ${ARGN}: expected ${expected}, got ${result}\n${error}")
  endif()
  set(output "${output}" PARENT_SCOPE)
  set(error "${error}" PARENT_SCOPE)
endfunction()

function(require_text text expected)
  string(FIND "${text}" "${expected}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "Missing expected output: ${expected}")
  endif()
endfunction()

run_compiler(0 --help)
require_text("${output}" "usage:")
run_compiler(0 --version)
require_text("${output}" "x23")
foreach(option -o --recursive_type --rust-feature --rust-no-feature)
  run_compiler(2 ${option})
  require_text("${error}" "missing")
endforeach()
run_compiler(2 --not-an-option)
require_text("${error}" "invalid option")
run_compiler(2)
require_text("${error}" "usage:")
run_compiler(1 "${TEST_DIR}/missing.x")
require_text("${error}" "missing.x")

# Compare stdout, explicit output and default output, including '--' and
# the joined -o form. Repeated runs must replace rather than append output.
run_compiler(0 --strict -o generated.hh "${INPUT}")
file(READ "${TEST_DIR}/generated.hh" cpp)
require_text("${cpp}" "using Name = x23::BoundedStr<LIMIT>;")
require_text("${cpp}" "std::array<std::int32_t, 2> fixed;")
require_text("${cpp}" "x23::Ptr<Record> next;")
require_text("${cpp}" "lookup = 1")
run_compiler(0 -o- "${INPUT}")
if(NOT output STREQUAL cpp)
  message(FATAL_ERROR "stdout differs from generated C++ header")
endif()
run_compiler(0 -- "${INPUT}")
file(READ "${TEST_DIR}/compiler.hh" default_cpp)
if(NOT default_cpp STREQUAL cpp)
  message(FATAL_ERROR "default output differs from explicit output")
endif()
file(WRITE "${TEST_DIR}/generated.hh" "stale output that must be replaced")
run_compiler(0 -ogenerated.hh "${INPUT}")
file(READ "${TEST_DIR}/generated.hh" regenerated)
if(NOT regenerated STREQUAL cpp)
  message(FATAL_ERROR "output was not replaced")
endif()
run_compiler(1 -o "${TEST_DIR}/missing/output.hh" "${INPUT}")
require_text("${error}" "output.hh")

run_compiler(0 --rust --rust-no-feature serde --rust-feature xdrs
  --recursive_type Record "${INPUT}")
file(READ "${TEST_DIR}/compiler.rs" rust)
require_text("${rust}" "pub struct Record")
require_text("${rust}" "pub next: Option<Box<Record>>")
require_text("${rust}" "pub payload: BytesM<LIMIT>")
require_text("${rust}" "#[derive(ReadXDR, WriteXDR, ConstXDRSize)]")
# Generated clients encode into a CallBuf taken from the sender and pass it
# to send_call_decoded by value, for zero-, one- and two-argument procedures.
# The xdrs writer runs inside CallBuf::append_with: thruput_rpc_api has no
# xdrs dependency, so the serializer call is the generated code's.
require_text("${rust}" "let mut call = self.handle.call_buf();")
require_text("${rust}" "call.append_with(|buf| {
             let mut w = xdrs::txs::buf_writer::BufWriter::from(::core::mem::take(buf));
             let r = xdrs::write_xdr(&(), &mut w);
             *buf = w.into();
             r
         }).map_err(|_| ::thruput_rpc_api::RpcError::GarbageArguments)?;")
require_text("${rust}" "let r = xdrs::write_xdr(&(arg0), &mut w);")
require_text("${rust}" "let r = xdrs::write_xdr(&(arg0, arg1), &mut w);")
string(FIND "${rust}" "call.encode(" encode_pos)
if(NOT encode_pos EQUAL -1)
  message(FATAL_ERROR "generated client still calls the removed CallBuf::encode")
endif()
# The reply is decoded by the sender, straight from the reader's buffer on
# driver connections, into a slot the method returns.
require_text("${rust}" "out = ::core::option::Option::Some(xdrs::txs::decode(reply)")
require_text("${rust}" "unsafe { self.handle.send_call_decoded(Self::prog_vers(), 2, call, &mut decode) }.await?;")
require_text("${rust}" "out.ok_or(::thruput_rpc_api::RpcError::SystemError)")
string(FIND "${rust}" "::rpc_api::" old_api_pos)
if(NOT old_api_pos EQUAL -1)
  message(FATAL_ERROR "generated Rust still names the old rpc_api crate")
endif()
# The borrowed read x23c writes out for a newtype fixed-opaque typedef is a
# plain impl: ReadXDR is a safe trait again as of xdrs 0.12.
require_text("${rust}" "impl<'r: 'a, 'a> ReadXDR<'r> for &'a Fingerprint {")
string(FIND "${rust}" "unsafe impl" unsafe_impl_pos)
if(NOT unsafe_impl_pos EQUAL -1)
  message(FATAL_ERROR "generated Rust contains an unsafe impl")
endif()
string(FIND "${rust}" "::serde::" serde_pos)
if(NOT serde_pos EQUAL -1)
  message(FATAL_ERROR "--rust-no-feature serde did not disable serde")
endif()

# #[rust(no_debug)] drops Debug from the derive list of the type carrying it,
# leaving the impl to hand-written code, and is stripped from the comment
# rather than passed through to the generated source.  Types without the
# marker still derive Debug.
require_text("${rust}" "#[derive(Clone, PartialEq, Eq, Hash, PartialOrd, Ord)]
#[derive(Default, Copy)]
#[repr(transparent)]
pub struct Fingerprint {")
require_text("${rust}" "#[derive(Clone, Debug, PartialEq, Eq, Hash, PartialOrd, Ord)]")
string(FIND "${rust}" "no_debug" no_debug_pos)
if(NOT no_debug_pos EQUAL -1)
  message(FATAL_ERROR "#[rust(no_debug)] marker leaked into the generated Rust")
endif()

run_compiler(0 --ts "${INPUT}")
file(READ "${TEST_DIR}/compiler.ts" ts)
require_text("${ts}" "export interface Record")
require_text("${ts}" "next: proto.Record | null;")
require_text("${ts}" "w.writeVarOpaque(val.payload, Number(proto.LIMIT));")
require_text("${ts}" "lookup: { proc: 1, request: () => Int32Codec, response: () => _Reply }")
require_text("${ts}" "} from \"@thruputchain/xdr\";")

file(WRITE "${TEST_DIR}/warning.x" "enum Warn { VALUE = 0, };\n")
run_compiler(0 -o warning.hh warning.x)
require_text("${error}" "warning:")
file(REMOVE "${TEST_DIR}/warning.hh")
run_compiler(1 --strict -o warning.hh warning.x)
require_text("${error}" "warnings treated as errors")
if(EXISTS "${TEST_DIR}/warning.hh")
  message(FATAL_ERROR "strict mode generated output after a warning")
endif()

# A failed write must be reported, not silently truncate the header.
if(EXISTS "/dev/full")
  run_compiler(1 -o /dev/full "${INPUT}")
  require_text("${error}" "/dev/full")
endif()

# A comment that shares a line with code, or a comment at end of file with
# no trailing newline, is not a whole-line comment, so it reaches x23c as a
# trailing comment.  Before the first token there is nothing to trail; it
# must become a leading comment rather than crash.
file(WRITE "${TEST_DIR}/leading_comment.x"
  "/* leading */ struct Lead { int value; };\n")
run_compiler(0 -o leading_comment.hh leading_comment.x)
file(READ "${TEST_DIR}/leading_comment.hh" leading_comment)
require_text("${leading_comment}" "/* leading */")
require_text("${leading_comment}" "struct Lead {")
file(WRITE "${TEST_DIR}/comment_only.x" "// nothing but a comment")
run_compiler(0 -o comment_only.hh comment_only.x)

# Comments opening a file that a blank line sets off from what follows are
# its header, such as an upstream copyright notice, and lead every
# generated file.  A comment touching the first definition still documents
# that definition.
file(WRITE "${TEST_DIR}/file_header.x"
  "// Copyright notice\n// License line\n\n/* Second header block */\n\n"
  "// Documents Head\nstruct Head { int value; };\n")
run_compiler(0 -o file_header.hh file_header.x)
file(READ "${TEST_DIR}/file_header.hh" file_header)
require_text("${file_header}" "// -*-C++-*-

// Copyright notice
// License line

/* Second header block */

#pragma once")
require_text("${file_header}" "// Documents Head\nstruct Head {")
run_compiler(0 --rust -o file_header.rs file_header.x)
file(READ "${TEST_DIR}/file_header.rs" file_header)
require_text("${file_header}" "// Copyright notice
// License line

/* Second header block */

#![allow(non_camel_case_types)]")
require_text("${file_header}" "// Documents Head\n#[cfg_attr(")
run_compiler(0 --ts -o file_header.ts file_header.x)
file(READ "${TEST_DIR}/file_header.ts" file_header)
require_text("${file_header}" "// Copyright notice
// License line

/* Second header block */

import {")
string(FIND "${file_header}" "Copyright notice" first_pos)
string(FIND "${file_header}" "Copyright notice" last_pos REVERSE)
if(NOT first_pos EQUAL last_pos)
  message(FATAL_ERROR "file header repeated in the generated TypeScript")
endif()

# A trailing comment runs on over the comment lines below it, but a doc
# comment there opens the next definition or field and must document it,
# not the one whose trailing comment it follows.
file(WRITE "${TEST_DIR}/trailing_comment.x"
  "/// Documents First.\ntypedef opaque First[4]; // trails First\n"
  "/// Documents Second.\ntypedef opaque Second[4];\n"
  "typedef int Third; // trails Third\n"
  "                   // and runs on\n"
  "struct Fourth {\n  int a; // trails a\n  /// Documents b.\n  int b;\n};\n")
run_compiler(0 -o trailing_comment.hh trailing_comment.x)
file(READ "${TEST_DIR}/trailing_comment.hh" trailing_comment)
require_text("${trailing_comment}" "/// Documents First.
// trails First
using First = std::array<std::uint8_t, 4>;")
require_text("${trailing_comment}" "/// Documents Second.
using Second = std::array<std::uint8_t, 4>;")
require_text("${trailing_comment}" "// trails Third
// and runs on
using Third = std::int32_t;")
require_text("${trailing_comment}" "  // trails a
  std::int32_t a;
  /// Documents b.
  std::int32_t b;")
run_compiler(0 --rust -o trailing_comment.rs trailing_comment.x)
file(READ "${TEST_DIR}/trailing_comment.rs" trailing_comment)
require_text("${trailing_comment}" "    pub a: i32,
    /// Documents b.
    pub b: i32,")

file(WRITE "${TEST_DIR}/invalid.x" "struct Broken { int value };\n")
run_compiler(1 -o invalid.hh invalid.x)
require_text("${error}" "syntax error")
file(WRITE "${TEST_DIR}/duplicate.x"
  "struct Same { int value; };\nstruct Same { int other; };\n")
run_compiler(1 -o duplicate.hh duplicate.x)
require_text("${error}" "Duplicate declaration of struct Same")

# A bool discriminant is validated on input, so a default arm in a
# bool-switched union becomes an explicit case for the value no case names.
file(WRITE "${TEST_DIR}/bool_default.x"
  "union BoolDefault switch (bool b) {\ncase TRUE:\n  int v;\ndefault:\n  string msg<>;\n};\n")
run_compiler(0 --strict -o bool_default.hh bool_default.x)
run_compiler(0 --strict --rust --rust-feature xdrs -o bool_default.rs bool_default.x)
file(READ "${TEST_DIR}/bool_default.rs" bool_default)
require_text("${bool_default}" "FALSE { msg: ")
string(FIND "${bool_default}" "default_variant" default_variant_pos)
if(NOT default_variant_pos EQUAL -1)
  message(FATAL_ERROR "bool union default arm left as a Rust default variant")
endif()
# The same holds through a typedef of bool.
file(WRITE "${TEST_DIR}/alias_default.x"
  "typedef bool Flag;\nunion AliasDefault switch (Flag f) {\ncase TRUE:\n  int v;\ndefault:\n  void;\n};\n")
run_compiler(0 --strict -o alias_default.hh alias_default.x)
file(READ "${TEST_DIR}/alias_default.hh" alias_default)
require_text("${alias_default}" "// case FALSE:")
# With both values named, a default arm can never match.
file(WRITE "${TEST_DIR}/bool_default_dead.x"
  "union BoolDead switch (bool b) {\ncase FALSE:\n  int x;\ncase TRUE:\n  int y;\ndefault:\n  void;\n};\n")
run_compiler(1 --rust -o bool_default_dead.rs bool_default_dead.x)
require_text("${error}" "default arm unreachable in union switched on bool")
# An enum-switched union keeps its default arm.
file(WRITE "${TEST_DIR}/enum_default.x"
  "enum Kind { A = 0, B = 1 };\nunion EnumDefault switch (Kind k) {\ncase A:\n  int v;\ndefault:\n  void;\n};\n")
run_compiler(0 --strict -o enum_default.hh enum_default.x)
file(READ "${TEST_DIR}/enum_default.hh" enum_default)
require_text("${enum_default}" "  default:\n")
