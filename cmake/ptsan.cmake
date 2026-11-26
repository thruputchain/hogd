# PointerTagSanitizer (PTSan) build mode; see docs/ptsan.md.
#
# PTSan is Aarno Labs' pointer-based memory-safety sanitizer. It ships as a
# fork of LLVM (https://github.com/Aarno-Labs/PointerTagSanitizer), so
# HOGD_PTSAN only works with that fork's clang as the C and C++ compiler.
# scripts/ptsan/build-toolchain.sh builds it; scripts/ptsan/build.sh
# configures hogd with it.
#
# Include this after the base compiler flags are set and before any
# add_subdirectory(): everything compiled into a hogd binary has to go
# through the PTSan compiler with the same mode flags.

option(HOGD_PTSAN
  "Build with Aarno Labs' PointerTagSanitizer (needs the PTSan clang)" OFF)
# One of the configurations the PTSan toolchain build generates next to its
# clang: ptsan-glibc.cfg (16-bit object IDs) or ptsan-glibc-id32.cfg (32-bit).
# It selects -fsanitize=ptsan, the ID width, and the matching libc++.
set(HOGD_PTSAN_CONFIG "ptsan-glibc.cfg" CACHE STRING
  "Clang --config file of the PTSan toolchain to build with")

if(NOT HOGD_PTSAN)
  return()
endif()

if(NOT CMAKE_C_COMPILER_ID STREQUAL "Clang" OR
   NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
  message(FATAL_ERROR
    "HOGD_PTSAN needs the PointerTagSanitizer clang as both CC and CXX "
    "(found ${CMAKE_C_COMPILER_ID} and ${CMAKE_CXX_COMPILER_ID}); "
    "use scripts/ptsan/build.sh")
endif()

# In the flag variables rather than add_compile_options, so that configure
# checks and nested builds compile against the PTSan libc++ as well. CMake
# also passes these when linking, which is where the driver picks the runtime.
string(APPEND CMAKE_C_FLAGS " --config=${HOGD_PTSAN_CONFIG}")
string(APPEND CMAKE_CXX_FLAGS " --config=${HOGD_PTSAN_CONFIG}")
# The configuration passes -nostdinc++ with its own libc++ include path, which
# leaves the -stdlib=libc++ that CMakeLists.txt adds unused in every compile.
add_compile_options(-Wno-unused-command-line-argument)

include(CheckCXXSourceCompiles)
check_cxx_source_compiles([=[
#if !__has_feature(pointer_tag_sanitizer)
#error "not a PointerTagSanitizer build"
#endif
int main() {}
]=] HOGD_PTSAN_COMPILER_WORKS)
if(NOT HOGD_PTSAN_COMPILER_WORKS)
  message(FATAL_ERROR
    "${CMAKE_CXX_COMPILER} cannot build with --config=${HOGD_PTSAN_CONFIG}. "
    "HOGD_PTSAN needs the PointerTagSanitizer clang and the configuration "
    "files its build generates; see docs/ptsan.md")
endif()
message(STATUS "HOGD_PTSAN: ON (${HOGD_PTSAN_CONFIG})")

# PTSan does not support -O0: the pass relies on mem2reg having run.
string(REPLACE "-O0" "-O1" CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG}")
string(REPLACE "-O0" "-O1" CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG}")

# Code PTSan cannot instrument must not receive tagged pointers.
#
# BLAKE3's default x86-64 backend is hand-written assembly; the intrinsics
# backend is C.
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64)$")
  set(BLAKE3_SIMD_TYPE "x86-intrinsics" CACHE STRING "" FORCE)
endif()
# PTSan interposes malloc itself, and the distribution's compression and
# io_uring libraries are uninstrumented. RocksDB is built without them, so a
# database written by a PTSan binary is uncompressed.
set(WITH_JEMALLOC OFF CACHE BOOL "" FORCE)
set(WITH_LIBURING OFF CACHE BOOL "" FORCE)
set(HOGD_ROCKSDB_COMPRESSION OFF)

# hogd_ptsan_shim_library(<target> <source>) builds one shim file as a static
# library whose link interface carries what the shims need from the linker:
# --wrap=f for every function f the file wraps, and --undefined=f for every
# function it interposes, which pulls the definition out of the archive even
# when only a shared library calls f. hogd/ptsan/shim.h explains shims.
function(hogd_ptsan_shim_library target source)
  get_filename_component(source "${source}" ABSOLUTE)
  set(link_options)
  foreach(kind IN ITEMS WRAPPED:wrap INTERPOSED:undefined)
    string(REPLACE ":" ";" kind "${kind}")
    list(GET kind 0 macro)
    list(GET kind 1 option)
    file(STRINGS "${source}" functions REGEX "^HOGD_PTSAN_${macro}\\(")
    list(TRANSFORM functions REPLACE
      "^HOGD_PTSAN_${macro}\\(([A-Za-z0-9_]+)\\).*" "-Wl,--${option}=\\1")
    list(APPEND link_options ${functions})
  endforeach()
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${source}")
  add_library(${target} STATIC "${source}")
  target_compile_options(${target} PRIVATE -fno-sanitize=ptsan)
  target_include_directories(${target} PRIVATE "${CMAKE_SOURCE_DIR}")
  target_link_options(${target} INTERFACE ${link_options})
endfunction()

# glibc functions the PTSan runtime does not wrap. Every target defined from
# here on links these shims, third-party ones included. BUILD_INTERFACE keeps
# the shims out of the export sets BLAKE3 and RocksDB install.
hogd_ptsan_shim_library(ptsan_libc_shims hogd/ptsan/libc_shims.cc)
link_libraries("$<BUILD_INTERFACE:ptsan_libc_shims>")

# The Rust crypto crate and the system libcrypto are uninstrumented as well;
# hogd/ptsan/CMakeLists.txt attaches their shims. Importing OpenSSL here, at
# the top level, makes this one OpenSSL::Crypto the target every directory
# links.
find_package(OpenSSL REQUIRED)

# hogd_ptsan_disable_ipo() turns interprocedural optimization off for every
# target defined so far in `dir` and below. Call it once all targets exist.
#
# PTSan 0.4.0 does not work under LTO. In a ThinLTO compile its pass runs
# before coroutines are lowered and stops on llvm.coro.id, and a full-LTO link
# crashes lld.
function(hogd_ptsan_disable_ipo dir)
  get_property(targets DIRECTORY "${dir}" PROPERTY BUILDSYSTEM_TARGETS)
  foreach(target IN LISTS targets)
    get_target_property(type ${target} TYPE)
    if(NOT type STREQUAL "INTERFACE_LIBRARY" AND NOT type STREQUAL "UTILITY")
      set_property(TARGET ${target} PROPERTY INTERPROCEDURAL_OPTIMIZATION FALSE)
    endif()
  endforeach()
  get_property(subdirs DIRECTORY "${dir}" PROPERTY SUBDIRECTORIES)
  foreach(subdir IN LISTS subdirs)
    hogd_ptsan_disable_ipo("${subdir}")
  endforeach()
endfunction()
