//! A wrapper around a storage object that only
//! needs to be stored once, upon contract initialization.
//!
//! (Objects with references, such as `Vec` and `Map`,
//! may allow manipulating sub-objects without re-storing
//! the root object.)

use crate::types::StorageKey;
use crate::storage::{load, store};
use core::cell::OnceCell;
use core::fmt::Debug;
use serde::{Serialize, de::DeserializeOwned};

// Maybe todo: A version of this type where the storage key is referenced or boxed,
// to save copying it onto the stack.

#[derive(Debug)]
pub struct ContractWithStorage<S> {
    key: StorageKey,
    storage: OnceCell<S>,
}

impl<S> ContractWithStorage<S>
    where S: Debug + Serialize + DeserializeOwned
{
    pub fn new(key: &StorageKey) -> Self {
        let storage = OnceCell::new();
        let key = key.clone();
        Self { key, storage }
    }

    pub fn storage_key(&self) -> &StorageKey {
        &self.key
    }

    /// Retrieve the storage value, getting it from contract
    /// storage if necessary.  Panic if `set_storage` was not
    /// called first.
    pub fn storage(&self) -> &S {
        self.storage.get().unwrap_or_else(|| {
            let s = load(&self.key).expect("Failed to load contract data");
            self.storage.set(s).expect("Storage may only be requested after `set_storage()` is called");
            self.storage.get().unwrap()
        })
    }

    /// Set the storage value.
    pub fn set_storage(&self, storage: S) {
        self.storage.set(storage).expect("Can't re-install storage");
        let storage = self.storage.get().unwrap();
        store(&self.key, storage).expect("Failed to store contract data");
    }
}
