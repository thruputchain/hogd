use alloc::{vec, vec::Vec};
use serde::{de::DeserializeOwned, Deserialize, Serialize};
use crate::crypto::{SHA256Hash, SHA256};
use crate::result::{with_context, Result};
use crate::serialize;
use crate::types::StorageKey;
use crate::{
    BytestringGet,
    BytestringSet,
    NonnegativeIntGet,
    NonnegativeIntAdd,
    NonnegativeIntSetAdd,
};

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
where
    T: Serialize,
{
    fn hash(&self) -> Result<Hash256> {
        let buf = serialize::to_bytes(&self)?;
        let hash = SHA256(&buf)?;
        Ok(hash)
    }
}

// ------------------- Storage -----------------

pub fn store<V: Serialize>(k: &StorageKey, v: &V) -> Result<()> {
    let buf = serialize::to_bytes(v)?;
    BytestringSet(k, buf.as_slice())?;
    Ok(())
}

pub fn load<V: DeserializeOwned>(k: &StorageKey) -> Result<V> {
    let buf = BytestringGet(k).map_err(with_context("BytestringGet"))?;
    let v = serialize::from_bytes(&*buf)?;
    Ok(v)
}

// -------------------- Keypath -----------------------

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct Keypath(Vec<Vec<u8>>);

impl Keypath {
    pub fn new() -> Self {
        Self(vec![])
    }

    pub fn from_storage_key(key: &StorageKey) -> Self {
        let mut keypath = Self::new();
        keypath.push_key(key);
        keypath
    }

    pub fn push_item<I: Serialize>(&mut self, item: &I) {
        self.0.push(serialize::to_bytes(item).unwrap());
    }

    pub fn push_key(&mut self, key: &StorageKey) {
        let mut item = Vec::with_capacity(key.len());
        item.extend_from_slice(key.as_slice());
        self.0.push(item);
    }

    pub fn as_key(&self) -> StorageKey {
        let mut info = vec![];
        for item in self.0.iter() {
            info.extend_from_slice(item.as_slice());
        }
        info.hash().unwrap()
    }

    pub fn cloned_push_item<I: Serialize>(&self, item: &I) -> Self {
        let mut res = self.clone();
        res.push_item(item);
        res
    }
}

impl Into<StorageKey> for Keypath {
    fn into(self) -> StorageKey {
        // We could do less copying with an incremental hash syscall
        let mut info = vec![];
        for item in self.0.into_iter() {
            info.extend_from_slice(item.as_slice());
        }
        info.hash().unwrap()
    }
}

impl From<StorageKey> for Keypath {
    fn from(key: StorageKey) -> Self {
        let mut keypath = Keypath::new();
        keypath.push_key(&key);
        keypath
    }
}

// -------------------- FromKeypath -----------------------

pub trait FromKeypath {
    fn from_keypath(keypath: Keypath) -> Result<Self>
        where Self: Sized;
}

// ------------------------- NNInt -----------------------

#[derive(Debug, Serialize, Deserialize)]
pub struct NNInt {
    keypath: Keypath,
}

impl FromKeypath for NNInt {
    fn from_keypath(keypath: Keypath) -> Result<Self> {
        Ok(NNInt { keypath })
    }
}

impl NNInt {
    pub fn get(&self) -> Result<(i128, i128)> {
        Ok(NonnegativeIntGet(&self.keypath.as_key())?)
    }

    pub fn get_current(&self) -> Result<i128> {
        let (set_value, add_value) = self.get()?;
        Ok(set_value + add_value)
    }

    pub fn try_get(&self) -> Result<Option<(i128, i128)>> {
        match self.get() {
            Err(e) if e.is_storage_key_not_exist() => Ok(None),
            other => other.map(Some)
        }
    }

    pub fn try_get_current(&self) -> Result<Option<i128>> {
        match self.get_current() {
            Err(e) if e.is_storage_key_not_exist() => Ok(None),
            other => other.map(Some)
        }
    }

    pub fn add(&mut self, add_value: i128) -> Result<()> {
        Ok(NonnegativeIntAdd(&self.keypath.as_key(), add_value)?)
    }

    pub fn set_add(&mut self, set_value: i128, add_value: i128) -> Result<()> {
        Ok(NonnegativeIntSetAdd(&self.keypath.as_key(), set_value, add_value)?)
    }
}

// ------------------- Map ---------------------

#[derive(Debug, Serialize, Deserialize)]
pub struct Map<K, V> {
    keypath: Keypath,
    phantom: core::marker::PhantomData<(K, V)>,
}

impl<K, V> FromKeypath for Map<K, V> {
    fn from_keypath(keypath: Keypath) -> Result<Self> {
        Ok(Self {
            keypath,
            phantom: core::marker::PhantomData,
        })
    }
}

impl<K, V> Map<K, V> {
    pub fn keypath(&self) -> &Keypath {
        &self.keypath
    }

    pub fn get(&self, entry_key: &K) -> Result<V>
    where
        K: Serialize,
        V: FromKeypath,
    {
        V::from_keypath(self.entry_keypath(entry_key))
    }

    fn entry_keypath(&self, entry_key: &K) -> Keypath
        where K: Serialize
    {
        self.keypath.cloned_push_item(entry_key)
    }
}
