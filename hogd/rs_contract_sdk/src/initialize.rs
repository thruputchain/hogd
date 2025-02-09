use crate::result::{Result, Error};
use crate::sys;
use crate::syscall::SyscallErrorCode;
use crate::types::StorageKey;
use crate::macros::static_key;

const INITIALIZE_GUARD_KEY: StorageKey =
    static_key!("initialization guard key");

// a function that can only execute on one transaction, ever
pub fn initialization_guard() -> Result<()> {
    let (mut current_set, mut current_add) = (0,0);
    match sys::raw_NonnegativeIntGet(
        &INITIALIZE_GUARD_KEY,
        &mut current_set,
        &mut current_add)
    {
        Err(x) => {
            if x.is_code(SyscallErrorCode::kStorageKeyNotExist) {
                // Ok, expected
            } else {
                panic!("Unexpected initialization failure: {}", x);
            }
        }
        _ => {
            return Err(Error::InitGuardFailure)
        }
    };

    crate::NonnegativeIntSetAdd(&INITIALIZE_GUARD_KEY, 1, -1)?;
    Ok(())
}
