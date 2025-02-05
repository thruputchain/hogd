mod contract_with_storage;
pub use contract_with_storage::*;

use crate::types::StorageKey;

pub trait Contract {
    fn new(key: &StorageKey) -> Self;
}
