// clang-format off

namespace hogd {
// clang-format on

// This enum is intended to match the ContractError enum in the SDK.
// This enables parsing errors returned from contracts into other languages.
//
// This would normally be generated here and then referenced in the SDK,
// but we get more ergonomic code by handling it as a special-case there,
// and then generating a corresponding enum here (needless to say,
// these need to stay in sync).
// Additionally, XDR does not have support for generics,
// so the full Result type cannot be defined here.

// ErrorMsg is a string in testing, but is compiled out to nothing
// when a contract is built to a wasm.

// SysErr is a int32, which is derived from a SyscallErrorCode.
typedef int SysErrXDR;

// Storage Error:
// pub enum Error {
//    #[error("Storage error due to {0}")]
//    Syscall(#[from] SysErr),
//
//    #[error("Storage error due to serialization")]
//    Serialization,
//
//    #[error("Vector index out of bounds: len={len}, access={access}")]
//    IndexOutOfBounds { len: u64, access: u64 },
//}

enum StorageErrorType {
  kStorageErrorTypeSyscall = 0,
  kStorageErrorTypeSerialization = 1,
  kStorageErrorTypeIndexOutOfBounds = 2,
};

struct IndexOutOfBoundsXDR {
  unsigned hyper len;
  unsigned hyper access;
};

union StorageErrXDR switch (StorageErrorType error_type)
{
case kStorageErrorTypeSyscall:
  SysErrXDR syscall_err;
case kStorageErrorTypeSerialization:
  void;
case kStorageErrorTypeIndexOutOfBounds:
  IndexOutOfBoundsXDR index_out_of_bounds_err;
};

// pub enum ContractError {
//     #[error(": {0}")]
//     Custom(ErrorMsg),
//
//     #[error(": failed due to syscall error: {0}")]
//     Syscall(#[from] SysErr),
//
//     #[cfg(feature = "storage")]
//     #[error(": failed due to storage error: {0}")]
//     Storage(#[from] StorageErr),
// }

enum ContractErrorType {
  kContractErrorTypeCustom = 0,
  kContractErrorTypeSyscall = 1,
  kContractErrorTypeStorage = 2,
};

union ContractErrorXDR switch (ContractErrorType error_type)
{
case kContractErrorTypeCustom:
  void; // ErrorMsg
case kContractErrorTypeSyscall:
  SysErrXDR syscall_err;
case kContractErrorTypeStorage:
  StorageErrXDR storage_err;
};

// Top level error returned from contract:
// pub enum Error {
//    #[error("Caller error{0}")]
//    Caller(ErrorMsg),
//
//    #[error("Contract internal error{0}")]
//    Contract(#[from] ContractError),
//}

enum ErrorType {
  kErrorTypeCaller = 0,
  kErrorTypeContract = 1,
};

union ErrorXDR switch (ErrorType error_type)
{
case kErrorTypeCaller:
  void; // ErrorMsg
case kErrorTypeContract:
  ContractErrorXDR contract_err;
};

// If XDR supported generics:
// union Result<T> switch (bool is_ok) {
//    case false:
//        T ok;
//    case true:
//        ErrorXDR err;
// };

} // namespace hogd
