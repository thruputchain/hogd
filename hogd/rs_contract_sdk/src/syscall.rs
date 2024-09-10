
pub use crate::crypto;

/**
 * SyscallNo is defined in designdocs/syscall.md
 * This should ideally be XDR-generated, but that's a long way off.
 */
#[repr(u64)]
pub enum SyscallNo {
    GetMsgSender = 0x0000,
    SHA256 = 0x0001,

    // storage obj read/write
    HasKey = 0x1000,
    DeleteLast = 0x1001,

    BytestringSet = 0x1100,
    BytestringGet = 0x1101,
    BytestringGetLen = 0x1102,
    NNInt64Set = 0x1200,
    NNInt64Get = 0x1201,
    NNInt64GetBase = 0x1202,

    // invoke
    GetCalldata = 0x2000,
    GetCalldataLen = 0x2001,
    Return = 0x2002,
    Invoke = 0x2003,
    GetFullReturn = 0x2004,
}

#[repr(u64)]
pub enum SyscallErrorCode {
    None = 0x0000_0000,
    IsRootTransaction = 0x0000_0001,
    StorageObjectTypeMismatch = 0x0000_0002,
    StorageKeyNExist = 0x0000_0003,
    StorageKeyAlreadyDeleted = 0x0000_0004,

    NonnegativeInt64SetValueMismatch = 0x0000_0005,

    DoubleReturn = 0x0000_0006,

    InvalidPtrError = 0x0000_0007,
    // anything negative is unrecoverable,
    // and can never be received by the sdk
}
// deriving PartialEq between SyscallErrorCode and u64 requires cloning values unnecessarily,

#[link(wasm_import_module = "marmot")]
extern "C" {
    pub(crate) fn syscall(syscall: u64, arg1: u64, arg2: u64, arg3: u64, arg4: u64) -> u64;
}


pub(crate) fn to_offset_mut<T>(obj: &mut T) -> *mut u8 {
    unsafe { core::mem::transmute(&obj) }
}

