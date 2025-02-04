use crate::types::StorageKey;
use crate::macros::static_key;
use crate::result::{Result, Error};

const INITIALIZE_GUARD_KEY: StorageKey =
    static_key!("initialization guard key");

// a function that can only execute on one transaction, ever
pub fn initialization_guard() -> Result<()> {
    let mut has_key_res: i32 = 0;
    match crate::syscall_bindings::raw_HasKey(
        &INITIALIZE_GUARD_KEY,
        &mut has_key_res,
    ) {
        Err(x) => {
            if x != rs_xdr::SyscallErrorCode::kStorageKeyNotExist as i32 {
                panic!("Unexpected HasKey failure: {}", x);
            }
        }
        _ => {
            return Err(Error::InitGuardFailure)
        }
    };

    crate::NonnegativeIntSetAdd(&INITIALIZE_GUARD_KEY, 1, -1)?;
    Ok(())
}
