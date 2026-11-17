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
run_compiler(2 -o)
require_text("${error}" "missing")
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

file(WRITE "${TEST_DIR}/invalid.x" "struct Broken { int value };\n")
run_compiler(1 -o invalid.hh invalid.x)
require_text("${error}" "syntax error")
file(WRITE "${TEST_DIR}/duplicate.x"
  "struct Same { int value; };\nstruct Same { int other; };\n")
run_compiler(1 -o duplicate.hh duplicate.x)
require_text("${error}" "Duplicate declaration of struct Same")
