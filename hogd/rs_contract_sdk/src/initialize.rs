use crate::macros::_static_key;
use crate::nonnegative_int::*;
use crate::result::{Error, Result};
use crate::types::StorageKey;

const INITIALIZE_GUARD_KEY: StorageKey =
    _static_key!("initialization guard key");

// a function that can only execute on one transaction, ever
pub fn initialization_guard() -> Result<()> {
    match NonnegativeIntGet(&INITIALIZE_GUARD_KEY) {
        Err(x) => {
            if x.is_storage_key_not_exist() {
                // Ok, expected
            } else {
                panic!("Unexpected initialization failure: {}", x);
            }
        }
        _ => return Err(Error::InitGuardFailure),
    };

    NonnegativeIntSetAdd(&INITIALIZE_GUARD_KEY, 1, -1)?;
    Ok(())
}
