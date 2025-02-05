use crate::types::StorageKey;
use crate::{
    BytestringGet,
    BytestringSet,
    NonnegativeIntGet,
};
use crate::crypto::{SHA256, SHA256Hash};
use crate::result::{Error, Result, with_context};
use crate::sys;
use serde::{Serialize, Deserialize, de::DeserializeOwned};
use bcs;

// ------------------ Hash ------------------

pub type Hash256 = SHA256Hash;

pub trait Hash {
    fn hash(&self) -> Result<Hash256>;
}

impl Hash for [u8] {
    fn hash(&self) -> Result<Hash256> {
        SHA256(self).map_err(From::from)
    }
}

impl<T> Hash for T
    where T: Serialize
{
    fn hash(&self) -> Result<Hash256> {
        let buf = bcs::to_bytes(&self)?;
        let hash = SHA256(&buf)?;
        Ok(hash)
    }
}

// ------------------- Storage -----------------

pub fn store<V: Serialize>(k: &StorageKey, v: &V) -> Result<()> {
    let buf = bcs::to_bytes(v)?;
    BytestringSet(k, buf.as_slice())?;
    Ok(())
}

pub fn load<V: DeserializeOwned>(k: &StorageKey) -> Result<V> {
    let buf = BytestringGet(k)?;
    let v = bcs::from_bytes(&*buf)?;
    Ok(v)
}

// -------------------- AsKey / FromKey -----------------------

pub trait AsKey {
    fn as_key(&self) -> &StorageKey;
}

pub trait FromKey {
    fn from_key(_: StorageKey) -> Self;
}

// ------------------------- NNInt -----------------------

#[derive(Debug, PartialEq, Serialize, Deserialize)]
pub struct NNInt {
    key: StorageKey
}

impl NNInt
{
    pub fn get_both(&self) -> Result<(i128, i128)> {
        NonnegativeIntGet(&self.key)
            .map_err(<Error as From<i32>>::from)
    }

    pub fn get(&self) -> Result<i128> {
        let (set, add) = self.get_both()?;
        Ok(set + add)
    }

    pub fn add(&mut self, add_value: i128) -> Result<()> {
        let (set, prev_add) = self.get_both()?;
        let new = prev_add + add_value;
        sys::raw_NonnegativeIntSetAdd(&self.key, &set, &new)
            .map_err(From::from)
    }

    pub fn set_add(&mut self, set_value: i128, add_value: i128) -> Result <()> {
        sys::raw_NonnegativeIntSetAdd(&self.key, &set_value, &add_value)
            .map_err(From::from)
    }
}

impl AsKey for NNInt {
    fn as_key(&self) -> &StorageKey {
        &self.key
    }
}

impl FromKey for NNInt {
    fn from_key(key: StorageKey) -> Self {
        NNInt { key }
    }
}

// ------------------- Map ---------------------

#[derive(Debug, PartialEq, Serialize, Deserialize)]
pub struct Map<K, V> {
    container_key: StorageKey,
    phantom: core::marker::PhantomData<(K, V)>
}

impl<K, V> AsKey for Map<K, V> {
    fn as_key(&self) -> &StorageKey {
        &self.container_key
    }
}

impl<K, V> FromKey for Map<K, V> {
    fn from_key(key: StorageKey) -> Self {
        Self {
            container_key: key,
            phantom: core::marker::PhantomData
        }
    }
}

impl<K, V> Map<K, V>
{
    pub fn get(&self, entry_key: &K) -> Result<V>
        where K: Serialize, V: DeserializeOwned
    {
        let value_key: StorageKey = try_item_key(&self.container_key, entry_key)?;
        load(&value_key)
    }

    pub fn insert(&self, entry_key: &K, value: &V) -> Result<()>
        where K: Serialize, V: Serialize
    {
        let value_key: StorageKey = try_item_key(&self.container_key, entry_key)?;
        store(&value_key, value)
    }
}

// ------------------- item_key ---------------------

/// Combine a key and some other data to form a subkey
pub fn try_item_key<I: Serialize>(container_key: &StorageKey, item_info: &I)
    -> Result<StorageKey>
{
    let item_key_info = (container_key, item_info);
    let item_key: StorageKey = item_key_info.hash()
        .map_err(with_context("hash failed"))?;
    Ok(item_key)
}

/// Combine a key and some other data to form a subkey
/// (panics if the SHA256 syscall fails)
pub fn item_key<I: Serialize>(container_key: &StorageKey, item_info: &I)
    -> StorageKey
{
    try_item_key(container_key, item_info)
        .expect("Couldn't create item key")
}
