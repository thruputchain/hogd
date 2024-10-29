use crate::rt::trap;
use crate::StorageKey;
use rs_macros::static_key;

const INITIALIZE_GUARD_KEY: StorageKey =
    static_key!("initialization guard key");

// a function that can only execute on one transaction, ever
pub fn initialization_guard() {
    let mut has_key_res: i32 = 0;
    match crate::syscall_bindings::raw_HasKey(
        &INITIALIZE_GUARD_KEY,
        &mut has_key_res,
    ) {
        Err(x) => {
            if x != rs_xdr::SyscallErrorCode::kStorageKeyNExist as i32 {
                trap();
            }
        }
        _ => {
            trap();
        }
    };

    crate::NonnegativeInt64SetAdd(&INITIALIZE_GUARD_KEY, 1, -1).unwrap();
}
