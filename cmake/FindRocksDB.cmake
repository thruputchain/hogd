# FindRocksDB.cmake
# Finds the RocksDB library
#
# This will define the following variables:
#   RocksDB_FOUND        - True if RocksDB is found
#   RocksDB_INCLUDE_DIRS - Include directories for RocksDB
#   RocksDB_LIBRARIES    - Libraries to link against
#
# and the following imported targets:
#   RocksDB::rocksdb

find_package(PkgConfig QUIET)
if(PKG_CONFIG_FOUND)
  pkg_check_modules(PC_RocksDB QUIET rocksdb)
endif()

# Find the header file
find_path(RocksDB_INCLUDE_DIR
  NAMES rocksdb/db.h
  HINTS
    ${PC_RocksDB_INCLUDE_DIRS}
  PATHS
    /usr/include
    /usr/local/include
    /opt/homebrew/include
    /opt/local/include
  PATH_SUFFIXES include
)

# Find the library
find_library(RocksDB_LIBRARY
  NAMES rocksdb
  HINTS
    ${PC_RocksDB_LIBRARY_DIRS}
  PATHS
    /usr/lib
    /usr/lib64
    /usr/local/lib
    /usr/local/lib64
    /opt/homebrew/lib
    /opt/local/lib
  PATH_SUFFIXES lib lib64
)

# Handle the QUIETLY and REQUIRED arguments
include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(RocksDB
  REQUIRED_VARS
    RocksDB_LIBRARY
    RocksDB_INCLUDE_DIR
  VERSION_VAR PC_RocksDB_VERSION
)

if(RocksDB_FOUND)
  set(RocksDB_LIBRARIES ${RocksDB_LIBRARY})
  set(RocksDB_INCLUDE_DIRS ${RocksDB_INCLUDE_DIR})
  
  # Create imported target
  if(NOT TARGET RocksDB::rocksdb)
    add_library(RocksDB::rocksdb UNKNOWN IMPORTED)
    set_target_properties(RocksDB::rocksdb PROPERTIES
      IMPORTED_LOCATION "${RocksDB_LIBRARY}"
      INTERFACE_INCLUDE_DIRECTORIES "${RocksDB_INCLUDE_DIR}"
    )
  endif()
  
  mark_as_advanced(RocksDB_INCLUDE_DIR RocksDB_LIBRARY)
  
  message(STATUS "Found RocksDB: ${RocksDB_LIBRARY}")
  message(STATUS "RocksDB include dir: ${RocksDB_INCLUDE_DIR}")
else()
  message(FATAL_ERROR "RocksDB not found! Please install RocksDB or specify its location.")
endif()
