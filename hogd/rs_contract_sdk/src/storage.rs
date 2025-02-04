use crate::types::StorageKey;
use crate::BytestringSet;
use crate::crypto::{SHA256, SHA256Hash};
use crate::result::{Error, Result, with_context};
use crate::io::{Read, Write, Cursor, Serialize, Deserialize, deserialize};
use crate::sys;

// Size of temporary buffers on the stack
// XXX: replace with static or allocated buffers
const TMP_SZ: usize = 1000;

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
        let mut cur = Cursor::new([0; TMP_SZ]);
        self.serialize(&mut cur)
            .map_err(with_context("Serialize failed"))?;
        let pos = cur.position();
        let buf = cur.into_inner();
        SHA256(&buf[0..pos])
            .map_err(From::from)
            .map_err(with_context("SHA256 failed"))
    }
}

// ------------------- Storage -----------------

pub fn store<V: Serialize>(k: &StorageKey, v: &V) -> Result<()> {
    let mut cur = Cursor::new([0; TMP_SZ]);
    v.serialize(&mut cur)?;
    let pos = cur.position();
    let buf = cur.into_inner();
    BytestringSet(k, &buf[0..pos]).map_err(From::from)
}

pub fn load<V: Deserialize>(k: &StorageKey) -> Result<V> {
    let mut buf = [0; TMP_SZ];
    let mut len = TMP_SZ as u32;
    sys::raw_BytestringGet(&k, &mut buf, &mut len)?;

    let mut cur = Cursor::new(&buf[0..len as usize]);
    deserialize(&mut cur)
}

// -------------------- AsKey / FromKey -----------------------

pub trait AsKey {
    fn as_key(&self) -> &StorageKey;
}

impl<T> Serialize for T
    where T: AsKey
{
    fn serialize<W: Write>(&self, writer: &mut W) -> Result<()> {
        self.as_key().serialize(writer)
    }
}

pub trait FromKey {
    fn from_key(_: StorageKey) -> Self;
}

impl<T> Deserialize for T
    where T: FromKey
{
    fn deserialize<R: Read>(reader: &mut R) -> Result<Self> {
        let key = StorageKey::deserialize(reader)?;
        Ok(Self::from_key(key))
    }
}

// ------------------------- NNInt -----------------------

#[derive(Debug, PartialEq)]
pub struct NNInt {
    key: StorageKey
}

impl NNInt
{
    pub fn get_both(&self) -> Result<(i128, i128)> {
        let mut set_result: i128 = 0;
        let mut add_result: i128 = 0;
        sys::raw_NonnegativeIntGet(
            &self.key,
            &mut set_result,
            &mut add_result,
        ).map_err(<Error as From<i32>>::from)?;
        Ok((set_result, add_result))
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

#[derive(Debug, PartialEq)]
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
        where K: Serialize, V: Deserialize
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
