// Library and storage exceptions from the contract SDK (`sdk_rs`).
//
// `FaultString` payloads in Rust are compiled out unless the `debugging` feature
// is enabled; on the wire they are empty, so every fault-string arm is `void`.
//
// Variant order matches `#[repr(i32)]` in sdk_rs (declaration order).
//
// Process from the hogd XDR tree so `%#include` paths resolve.

%#include "types.hh"
%#include "syscall.hh"

namespace hogd {

/// Syscall error number (`sdk::sys::SysErr`).
typedef int SysErr;

/// Syscall failure: which call failed and the error number.
struct SyscallExn {
  SyscallNo call_number;
  SysErr err_number;
};

// --- HandlingError (sdk_rs `exn::wrapper::HandlingError`) ---

enum HandlingErrorKind {
  kHandlingDecodingArgs = 0,
  kHandlingEncodingSuccess = 1,
  kHandlingEncodingException = 2,
  kHandlingEncodingEvent = 3
};

union HandlingError switch (HandlingErrorKind kind) {
case kHandlingDecodingArgs:
  void;
case kHandlingEncodingSuccess:
  void;
case kHandlingEncodingException:
  void;
case kHandlingEncodingEvent:
  void;
};

// --- CallingError (sdk_rs `exn::wrapper::CallingError`) ---

enum CallingErrorKind {
  kCallingDecodingSuccess = 0,
  kCallingDecodingException = 1,
  kCallingEncodingArgs = 2,
  kCallingSyscall = 3
};

union CallingError switch (CallingErrorKind kind) {
case kCallingDecodingSuccess:
  void;
case kCallingDecodingException:
  void;
case kCallingEncodingArgs:
  void;
case kCallingSyscall:
  SysErr syscall_err;
};

// --- StorageError (sdk_rs `storage::error::StorageError`) ---

struct StorageErrorIndexOutOfBounds {
  unsigned hyper len;
  unsigned hyper access;
};

enum StorageErrorKind {
  kStorageSys = 0,
  kStorageSerialization = 1,
  kStorageIndexOutOfBounds = 2
};

union StorageError switch (StorageErrorKind kind) {
case kStorageSys:
  SyscallExn exn;
case kStorageSerialization:
  void;
case kStorageIndexOutOfBounds:
  StorageErrorIndexOutOfBounds index_out_of_bounds;
};

// --- LibraryExn (sdk_rs `exn::library::LibraryExn`) ---

struct LibraryExnBytestringWrongSize {
  unsigned int expected;
  unsigned int found;
};

enum LibraryExnKind {
  kLibraryExnSyscall = 0,
  kLibraryExnHandling = 1,
  kLibraryExnCalling = 2,
  kLibraryExnCalled = 3,
  kLibraryExnBytestringWrongSize = 4,
  kLibraryExnNonNegativeIntUnderflow = 5,
  kLibraryExnNonNegativeIntOverflow = 6,
  kLibraryExnStorage = 7
};

union LibraryExn switch (LibraryExnKind kind) {
case kLibraryExnSyscall:
  SyscallExn syscall_exn;
case kLibraryExnHandling:
  HandlingError handling;
case kLibraryExnCalling:
  CallingError calling;
case kLibraryExnCalled:
  void;
case kLibraryExnBytestringWrongSize:
  LibraryExnBytestringWrongSize bytestring_wrong_size;
case kLibraryExnNonNegativeIntUnderflow:
  void;
case kLibraryExnNonNegativeIntOverflow:
  void;
case kLibraryExnStorage:
  StorageError storage;
};

} // namespace hogd
