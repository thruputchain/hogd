macro(generate_cmake_repro_string OUTVAR)
    # Prefix all internal variables to avoid collisions
    set(_REPRO_VARS
        CMAKE_C_COMPILER
        CMAKE_CXX_COMPILER
        CMAKE_C_FLAGS
        CMAKE_CXX_FLAGS
        CMAKE_EXE_LINKER_FLAGS
        CMAKE_SHARED_LINKER_FLAGS
        CMAKE_MODULE_LINKER_FLAGS
        CMAKE_BUILD_TYPE
        CMAKE_TOOLCHAIN_FILE
#        CMAKE_GENERATOR
        CMAKE_GENERATOR_PLATFORM
        CMAKE_GENERATOR_TOOLSET
    )

    # Build flag list
    set(_REPRO_FLAGS "")
    foreach(_var IN LISTS _REPRO_VARS)
        if(DEFINED ${_var} AND NOT "${${_var}}" STREQUAL "")
            set(_val "${${_var}}")
            string(REPLACE "'" "'\"'\"'" _val_esc "${_val}")
            string(APPEND _REPRO_FLAGS " -D${_var}='${_val_esc}'")
        endif()
    endforeach()

    # Reconstruct original CMake invocation
    set(_INVOCATION "")
    foreach(_i RANGE 0 ${CMAKE_ARGC}-1)
        set(_arg "${CMAKE_ARGV${_i}}")
        if("${_arg}" STREQUAL "")
            continue()
        endif()
        string(REPLACE "'" "'\"'\"'" _arg_esc "${_arg}")
        string(APPEND _INVOCATION " '${_arg_esc}'")
    endforeach()
    string(STRIP "${_INVOCATION}" _INVOCATION)

    # Output variable goes to caller scope - this is safe in a macro
    set(${OUTVAR} "cmake${_REPRO_FLAGS} ${_INVOCATION}")
endmacro()


# ============================================================
#  git_describe_string(<outvar> [<path>] [NODIRTY])
#
#  Sets <outvar> to:
#      git describe --always --abbrev=999 --dirty=+dirty
#
#  Options:
#    - If <path> omitted -> ${CMAKE_SOURCE_DIR}
#    - If <path> relative -> relative to ${CMAKE_SOURCE_DIR}
#    - If NODIRTY present -> suppress +dirty
# ============================================================
macro(git_describe_string OUTVAR)
    find_package(Git QUIET)

    # --- Parse arguments ---
    set(_path "")
    set(_nodirty FALSE)

    if(${ARGC} GREATER 1)
        # See if last arg is NODIRTY
        if("${ARGV${ARGC}-1}" STREQUAL "NODIRTY")
            set(_nodirty TRUE)
            # path may or may not exist before NODIRTY
            if(${ARGC} GREATER 2)
                set(_path "${ARGV1}")
            endif()
        else()
            set(_path "${ARGV1}")
        endif()
    endif()

    # Default path = root source dir
    if("${_path}" STREQUAL "")
        set(_path "${CMAKE_SOURCE_DIR}")
    endif()

    # Convert relative paths to absolute
    if(NOT IS_ABSOLUTE "${_path}")
        set(_path "${CMAKE_SOURCE_DIR}/${_path}")
    endif()

    # --- Run git describe ---
    if(Git_FOUND)
        if(_nodirty)
            # No +dirty suffix
            execute_process(
                COMMAND "${GIT_EXECUTABLE}" describe --always --abbrev=999
                WORKING_DIRECTORY "${_path}"
                OUTPUT_VARIABLE _gitdesc
                OUTPUT_STRIP_TRAILING_WHITESPACE
                ERROR_QUIET
            )
        else()
            # Normal output including +dirty
            execute_process(
                COMMAND "${GIT_EXECUTABLE}" describe --always --abbrev=999 --dirty=+dirty
                WORKING_DIRECTORY "${_path}"
                OUTPUT_VARIABLE _gitdesc
                OUTPUT_STRIP_TRAILING_WHITESPACE
                ERROR_QUIET
            )
        endif()

        if(NOT _gitdesc)
            set(_gitdesc "unknown")
        endif()
    else()
        set(_gitdesc "git-not-found")
    endif()

    # Export result
    set(${OUTVAR} "${_gitdesc}")
endmacro()
