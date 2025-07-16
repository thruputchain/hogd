  
find_program(WASMOPT_EXECUTABLE NAMES wasm-opt DOC "path to the wasm-opt executable")

macro(WASMOPT_TARGET Name Input Output)

  set(WASMOPT_TARGET_PARAM_OPTIONS)
  set(WASMOPT_TARGET_PARAM_ONE_VALUE_KEYWORDS)
  set(WASMOPT_TARGET_PARAM_MULTI_VALUE_KEYWORDS)

  cmake_parse_arguments(
    WAT2WASM_TARGET_ARG
    "${WAT2WASM_TARGET_PARAM_OPTIONS}"
    "${WAT2WASM_TARGET_PARAM_ONE_VALUE_KEYWORDS}"
    "${WAT2WASM_TARGET_MULTI_VALUE_KEYWORDS}"
    ${ARGN}
    )

  set(WAT2WASM_TARGET_usage "WASMOPT_TARGET(<Name> <Input> <Output> [flags for wasm-opt])")

  if(NOT "${WASMOPT_TARGET_ARG_UNPARSED_ARGUMENTS}" STREQUAL "")
    message(SEND_ERROR ${WASMOPT_TARGET_usage})
  else()

    set(_wasmopt_FLAGS ${ARGN})

    set(_wasmopt_INPUT "${Input}")
    set(_wasmopt_WORKING_DIR "${CMAKE_CURRENT_BINARY_DIR}")
    if(NOT IS_ABSOLUTE "${_wasmopt_INPUT}")
        set(_wasmopt_INPUT "${CMAKE_CURRENT_SOURCE_DIR}/${_wasmopt_INPUT}")
    endif()

    set(_wasmopt_OUTPUT "${Output}")
    if(NOT IS_ABSOLUTE ${_wasmopt_OUTPUT})
        set(_wasmopt_OUTPUT "${_wasmopt_WORKING_DIR}/${_wasmopt_OUTPUT}")
    endif()
    set(_wasmopt_TARGET_OUTPUTS "${_wasmopt_OUTPUT}")

    get_filename_component(_wasmopt_EXE_NAME_WE "${WASMOPT_EXECUTABLE}" NAME_WE)
    add_custom_command(OUTPUT ${_wasmopt_TARGET_OUTPUTS}
        COMMAND ${WASMOPT_EXECUTABLE} ${_wasmopt_INPUT} ${_wasmopt_FLAGS} -o ${_wasmopt_OUTPUT}
        VERBATIM
        DEPENDS ${_wasmopt_INPUT}
        COMMENT "[wasm-opt][${Name}] Optimize with ${_wasmopt_EXE_NAME_WE} ${_wasmopt_FLAGS}"
        WORKING_DIRECTORY ${_wasmopt_WORKING_DIR})

    set(WASMOPT_${Name}_DEFINED TRUE)
    set(WASMOPT_${Name}_OUTPUTS ${_wasmopt_TARGET_OUTPUTS})
    set(WASMOPT_${Name}_INPUT ${_wasmopt_INPUT})
    set(WASMOPT_${Name}_FLAGS ${_wasmopt_FLAGS})

    add_custom_target(${Name} DEPENDS ${_wasmopt_TARGET_OUTPUTS})

    unset(_wasmopt_EXE_NAME_WE)
    unset(_wasmopt_INPUT)
    unset(_wasmopt_OUTPUT)
    unset(_wasmopt_TARGET_OUTPUTS)
    unset(_wasmopt_WORKING_DIR)
    unset(_wasmopt_FLAGS)
  endif()

endmacro()
