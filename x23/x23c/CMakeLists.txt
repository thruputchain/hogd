find_package(FLEX)
if(NOT FLEX_FOUND)
  message(FATAL_ERROR "flex required to build x23c")
endif()

find_package(BISON)
if(NOT BISON_FOUND)
  message(FATAL_ERROR "bison required to build x23c")
endif()

add_custom_command(
  OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/lex.cc ${CMAKE_CURRENT_BINARY_DIR}/lex.hh
  COMMAND ${FLEX_EXECUTABLE}
  ARGS ${CMAKE_CURRENT_SOURCE_DIR}/lex.ll
  DEPENDS ${FLEX_INPUT} ${CMAKE_CURRENT_BINARY_DIR}/parse.hh
  WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/..
  COMMENT "Generating lexer"
)

add_custom_command(
  OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/parse.cc ${CMAKE_CURRENT_BINARY_DIR}/parse.hh
  COMMAND ${BISON_EXECUTABLE}
  ARGS ${CMAKE_CURRENT_SOURCE_DIR}/parse.yy
  DEPENDS ${BISON_INPUT}
  WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/..
  COMMENT "Generating parser"
)

add_custom_target(generate_parser_and_lexer DEPENDS
  ${CMAKE_CURRENT_BINARY_DIR}/parse.hh
  ${CMAKE_CURRENT_BINARY_DIR}/parse.cc
  ${CMAKE_CURRENT_BINARY_DIR}/lex.hh
  ${CMAKE_CURRENT_BINARY_DIR}/lex.cc
)

add_executable(x23c
  defs.h
  ${CMAKE_CURRENT_BINARY_DIR}/parse.cc
  ${CMAKE_CURRENT_BINARY_DIR}/lex.cc
  gen_hh.cc
  gen_rs.cc
  x23c.cc
)

add_dependencies(x23c generate_parser_and_lexer)

target_include_directories(x23c PRIVATE
  ${X23_SOURCE_DIR}
  ${X23_BINARY_DIR}
)

if(X23_IS_TOP_LEVEL)
  install(TARGETS x23c)
endif()
