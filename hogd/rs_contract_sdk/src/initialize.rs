use crate::general_storage::HasKey;
use crate::rt::trap;
use crate::StorageKey;
use rs_macros::static_key;

const INITIALIZE_GUARD_KEY: StorageKey =
    static_key!("initialization guard key");

// a function that can only execute on one transaction, ever
pub fn initialization_guard() {
    match HasKey(&INITIALIZE_GUARD_KEY) {
        Err(_) => {
            trap();
        }
        _ => (),
    };

    crate::NonnegativeInt64SetAdd(&INITIALIZE_GUARD_KEY, 1, -1).unwrap();
}
