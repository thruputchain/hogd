use crate::storage::Keypath;
use crate::storage::Load;

pub trait ContractStorage: Load {
    /// The keypath to the root storage object
    fn keypath() -> Keypath {
        let mut keypath = Keypath::new();
        keypath.push_bytes(&[42]);
        keypath
    }
}

// Load contract storage via its associated function `keypath()`.
// Panics for any error besides key-not-present.
// Used by macros to load contract state before invoking methods.
#[doc(hidden)]
pub fn try_load_state<S>() -> Option<S>
where
    S: ContractStorage,
{
    let keypath = <S as ContractStorage>::keypath();
    <S as Load>::try_load_keypath(&keypath)
        .expect("Failed loading root keypath")
}
