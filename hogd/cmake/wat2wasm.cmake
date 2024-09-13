  
find_program(WAT2WASM_EXECUTABLE NAMES wat2wasm DOC "path to the wat2wasm executable")

# Basically copied from the flex target demo
macro(WAT2WASM_TARGET Name Input Output)

  set(WAT2WASM_TARGET_PARAM_OPTIONS)
  set(WAT2WASM_TARGET_PARAM_ONE_VALUE_KEYWORDS
    DEFINES_FILE
  )
  set(WAT2WASM_TARGET_PARAM_MULTI_VALUE_KEYWORDS)

  cmake_parse_arguments(
    WAT2WASM_TARGET_ARG
    "${WAT2WASM_TARGET_PARAM_OPTIONS}"
    "${WAT2WASM_TARGET_PARAM_ONE_VALUE_KEYWORDS}"
    "${WAT2WASM_TARGET_MULTI_VALUE_KEYWORDS}"
    ${ARGN}
    )

  set(WAT2WASM_TARGET_usage "WAT2WASM_TARGET(<Name> <Input> <Output> [DEFINES_FILE <string>]")

  if(NOT "${WAT2WASM_TARGET_ARG_UNPARSED_ARGUMENTS}" STREQUAL "")
    message(SEND_ERROR ${WAT2WASM_TARGET_usage})
  else()
      
      set(_wat2wasm_INPUT "${Input}")
      set(_wat2wasm_WORKING_DIR "${CMAKE_CURRENT_BINARY_DIR}")
      if(NOT IS_ABSOLUTE "${_wat2wasm_INPUT}")
        set(_wat2wasm_INPUT "${CMAKE_CURRENT_SOURCE_DIR}/${_wat2wasm_INPUT}")
      endif()
      
      set(_wat2wasm_OUTPUT "${Output}")
      if(NOT IS_ABSOLUTE ${_wat2wasm_OUTPUT})
        set(_wat2wasm_OUTPUT "${_wat2wasm_WORKING_DIR}/${_wat2wasm_OUTPUT}")
      endif()
      set(_wat2wasm_TARGET_OUTPUTS "${_wat2wasm_OUTPUT}")

      set(_wat2wasm_OUTPUT_HEADER "")
      if(NOT "${WAT2WASM_TARGET_ARG_DEFINES_FILE}" STREQUAL "")
        set(_wat2wasm_OUTPUT_HEADER "${WAT2WASM_TARGET_ARG_DEFINES_FILE}")
        if(IS_ABSOLUTE "${_wat2wasm_OUTPUT_HEADER}")
          set(_wat2wasm_OUTPUT_HEADER_ABS "${_wat2wasm_OUTPUT_HEADER}")
        else()
          set(_wat2wasm_OUTPUT_HEADER_ABS "${_wat2wasm_WORKING_DIR}/${_wat2wasm_OUTPUT_HEADER}")
        endif()
        list(APPEND _wat2wasm_TARGET_OUTPUTS "${_wat2wasm_OUTPUT_HEADER_ABS}")
        list(APPEND _wat2wasm_EXE_OPTS --header-file=${_wat2wasm_OUTPUT_HEADER_ABS})
      endif()

      get_filename_component(_wat2wasm_EXE_NAME_WE "${WAT2WASM_EXECUTABLE}" NAME_WE)
      add_custom_command(OUTPUT ${_wat2wasm_TARGET_OUTPUTS}
        COMMAND ${WAT2WASM_EXECUTABLE} ${_wat2wasm_EXE_OPTS} ${_wat2wasm_INPUT} -o ${_wat2wasm_OUTPUT}
        VERBATIM
        DEPENDS ${_wat2wasm_INPUT}
        COMMENT "[WAT2WASM][${Name}] Wat to Wasm with ${_wat2wasm_EXE_NAME_WE} ${WAT2WASM_VERSION}"
        WORKING_DIRECTORY ${_wat2wasm_WORKING_DIR})

      set(WAT2WASM_${Name}_DEFINED TRUE)
      set(WAT2WASM_${Name}_OUTPUTS ${_wat2wasm_TARGET_OUTPUTS})
      set(WAT2WASM_${Name}_INPUT ${_watm2wasm_INPUT})
      set(WAT2WASM_${Name}_OUTPUT_HEADER ${_wat2wasm_OUTPUT_HEADER})
      
      add_custom_target(${Name} DEPENDS ${_wat2wasm_TARGET_OUTPUTS})

      unset(_wat2wasm_EXE_NAME_WE)
      unset(_wat2wasm_EXE_OPTS)
      unset(_wat2wasm_INPUT)
      unset(_wat2wasm_OUTPUT)
      unset(_wat2wasm_OUTPUT_HEADER)
      unset(_wat2wasm_OUTPUT_HEADER_ABS)
      unset(_wat2wasm_TARGET_OUTPUTS)
      unset(_wat2wasm_WORKING_DIR)

    endif()
  endmacro()
