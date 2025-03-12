use crate::result::Result;

pub use hash::*;
mod hash {
    use super::Result;
    use crate::crypto::{SHA256Hash, SHA256};
    use crate::serialize::{self, Serialize};

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
}

pub use load::*;
mod load {
    use super::hash::Hash;
    use super::Result;
    use crate::bytestring::{BytestringGet, BytestringSet};
    use crate::serialize::{self, DeserializeOwned, Serialize};
    use crate::types::StorageKey;
    use alloc::{vec, vec::Vec};
    use core::fmt::Debug;

    pub fn store<V: Serialize>(k: &StorageKey, v: &V) -> Result<()> {
        let buf = serialize::to_bytes(v)?;
        BytestringSet(k, buf.as_slice())?;
        Ok(())
    }

    pub fn load<V: DeserializeOwned>(k: &StorageKey) -> Result<V> {
        let buf = BytestringGet(k)?;
        let v = serialize::from_bytes(&*buf)?;
        Ok(v)
    }

    pub fn try_load<V: DeserializeOwned>(k: &StorageKey) -> Result<Option<V>> {
        match load(k) {
            Ok(v) => Ok(Some(v)),
            Err(error) => {
                if error.is_storage_key_not_exist() {
                    Ok(None)
                } else {
                    Err(error)
                }
            }
        }
    }

    #[derive(Debug, Clone)]
    pub enum KeypathComponent {
        Bytes(Vec<u8>),
        Index(u32),
    }

    #[derive(Debug, Clone, Serialize)]
    pub struct Keypath {
        #[serde(skip)]
        path: Vec<KeypathComponent>,
    }

    impl Keypath {
        pub fn new() -> Self {
            Self { path: vec![] }
        }

        pub fn generate_key(&self) -> StorageKey {
            let mut info = vec![];
            for item in self.path.iter() {
                match item {
                    KeypathComponent::Bytes(v) => {
                        // Encapsulate (e.g. with length tag) to avoid ambiguity
                        let v = serialize::to_bytes(v).unwrap();
                        info.extend_from_slice(v.as_slice());
                    }
                    KeypathComponent::Index(i) => {
                        let v = serialize::to_bytes(i).unwrap();
                        info.extend_from_slice(v.as_slice());
                    }
                }
            }
            info.hash().unwrap()
        }

        pub fn push_bytes(&mut self, slice: &[u8]) {
            let mut v = vec![];
            v.extend_from_slice(slice);
            self.path.push(KeypathComponent::Bytes(v));
        }

        pub fn push_item<I: Serialize>(&mut self, item: &I) {
            self.push_bytes(serialize::to_bytes(item).unwrap().as_slice())
        }

        pub fn enter_seq(&mut self) {
            self.path.push(KeypathComponent::Index(0));
        }

        pub fn seq_next(&mut self) {
            if let Some(KeypathComponent::Index(i)) = self.path.pop() {
                self.path.push(KeypathComponent::Index(i + 1))
            } else {
                panic!("Keypath: seq_next() called when last path component is not an index");
            }
        }

        pub fn leave_seq(&mut self) {
            if !matches!(self.path.pop(), Some(KeypathComponent::Index(_))) {
                panic!("Keypath: leave_seq() called when last path component is not an index");
            }
        }
    }

    impl Into<StorageKey> for Keypath {
        fn into(self) -> StorageKey {
            self.generate_key()
        }
    }

    /// Can be loaded from a [`Keypath`]
    pub trait Load: Sized {
        /// Load the value at the given keypath
        fn load_keypath(keypath: &Keypath) -> Result<Self>;

        /// Implemented in terms of `load_keypath`
        fn try_load_keypath(keypath: &Keypath) -> Result<Option<Self>> {
            Self::load_keypath(keypath).map(Some).or_else(|error| {
                if error.is_storage_key_not_exist() {
                    Ok(None)
                } else {
                    Err(error)
                }
            })
        }
    }

    /* This conflicts with the implementation for T: DeserializeOwned,
     * which is convenient for clients.
    impl<T> Load for T
    where
        T: From<Keypath>,
    {
        fn load_keypath(keypath: &Keypath) -> Result<Self> {
            // No I/O necessary
            Ok(From::from(keypath.clone()))
        }
    }
    */

    impl<T> Load for T
    where
        T: DeserializeOwned,
    {
        fn load_keypath(keypath: &Keypath) -> Result<Self> {
            // Load from a storage bytesting
            let key: StorageKey = keypath.generate_key();
            load(&key)
        }
    }

    pub struct KeypathSeed<T> {
        pub keypath: Keypath,
        pub marker: core::marker::PhantomData<T>,
    }

    impl<T> KeypathSeed<T> {
        pub fn new(keypath: &Keypath) -> Self {
            Self {
                keypath: keypath.clone(),
                marker: core::marker::PhantomData,
            }
        }
    }
}

pub use bytestring::*;
mod bytestring {
    use super::load::Keypath;
    use crate::serialize::Serialize;
    use core::fmt::Debug;

    #[derive(Debug, Serialize)]
    pub struct Bytestring {
        keypath: Keypath,
    }

    impl From<Keypath> for Bytestring {
        fn from(keypath: Keypath) -> Self {
            Self { keypath }
        }
    }
}

pub use nnint::*;
mod nnint {
    use super::load::Keypath;
    use super::Result;
    use crate::nonnegative_int::{
        NonnegativeIntAdd, NonnegativeIntGet, NonnegativeIntSetAdd,
    };
    use crate::serialize::Serialize;
    use core::fmt::Debug;

    #[derive(Debug, Serialize)]
    pub struct NNInt {
        keypath: Keypath,
    }

    impl From<Keypath> for NNInt {
        fn from(keypath: Keypath) -> Self {
            Self { keypath }
        }
    }

    impl NNInt {
        pub fn new(keypath: &Keypath) -> Self {
            Self {
                keypath: keypath.clone(),
            }
        }

        pub fn get(&self) -> Result<(i128, i128)> {
            Ok(NonnegativeIntGet(&self.keypath.generate_key())?)
        }

        pub fn get_current(&self) -> Result<i128> {
            let (set_value, add_value) = self.get()?;
            Ok(set_value + add_value)
        }

        pub fn try_get(&self) -> Result<Option<(i128, i128)>> {
            match self.get() {
                Err(e) if e.is_storage_key_not_exist() => Ok(None),
                other => other.map(Some),
            }
        }

        pub fn try_get_current(&self) -> Result<Option<i128>> {
            match self.get_current() {
                Err(e) if e.is_storage_key_not_exist() => Ok(None),
                other => other.map(Some),
            }
        }

        pub fn add(&mut self, add_value: i128) -> Result<()> {
            Ok(NonnegativeIntAdd(&self.keypath.generate_key(), add_value)?)
        }

        pub fn set_add(
            &mut self,
            set_value: i128,
            add_value: i128,
        ) -> Result<()> {
            Ok(NonnegativeIntSetAdd(
                &self.keypath.generate_key(),
                set_value,
                add_value,
            )?)
        }
    }

    mod deserialize_impl {
        use super::NNInt;
        use crate::storage::{Keypath, KeypathSeed, Load};
        use core::fmt;
        use serde::de::{DeserializeSeed, Visitor};
        use serde::Deserializer;

        impl<'de> DeserializeSeed<'de> for KeypathSeed<NNInt> {
            type Value = NNInt;

            fn deserialize<D>(
                self,
                deserializer: D,
            ) -> Result<Self::Value, D::Error>
            where
                D: Deserializer<'de>,
            {
                struct KeypathVisitor<'a> {
                    keypath: &'a mut Keypath,
                }

                impl<'de, 'a> Visitor<'de> for KeypathVisitor<'a> {
                    type Value = NNInt;

                    fn expecting(
                        &self,
                        formatter: &mut fmt::Formatter,
                    ) -> fmt::Result {
                        write!(formatter, "an NNInt")
                    }

                    fn visit_unit<E>(self) -> Result<Self::Value, E> {
                        Ok(NNInt::new(self.keypath))
                    }
                }

                let mut keypath = self.keypath.clone();
                let visitor = KeypathVisitor {
                    keypath: &mut keypath,
                };
                deserializer.deserialize_unit(visitor)
            }
        }

        impl Load for NNInt {
            fn load_keypath(keypath: &Keypath) -> crate::result::Result<Self> {
                Ok(From::from(keypath.clone()))
            }
        }
    }
}

pub use map::*;
mod map {
    use super::load::{Keypath, Load};
    use super::Result;
    use crate::serialize::Serialize;
    use core::fmt::Debug;

    #[derive(Debug, Serialize)]
    pub struct Map<K, V> {
        keypath: Keypath,
        phantom: core::marker::PhantomData<(K, V)>,
    }

    // TODO: impl Index

    impl<K, V> From<Keypath> for Map<K, V> {
        fn from(keypath: Keypath) -> Self {
            Self {
                keypath,
                phantom: core::marker::PhantomData,
            }
        }
    }

    impl<K, V> Map<K, V> {
        pub fn new(keypath: &Keypath) -> Self {
            Self {
                keypath: keypath.clone(),
                phantom: core::marker::PhantomData,
            }
        }

        pub fn keypath(&self) -> &Keypath {
            &self.keypath
        }

        pub fn get(&self, entry_key: &K) -> Result<V>
        where
            K: Serialize,
            V: Load,
        {
            V::load_keypath(&self.make_entry_keypath(entry_key))
        }

        // TODO: have get() return a shared ref to the entry
        // (itself a shared ref to the map, plus the entry_key),
        // and get_mut() returns a mutable ref

        fn make_entry_keypath(&self, entry_key: &K) -> Keypath
        where
            K: Serialize,
        {
            let mut keypath = self.keypath.clone();
            keypath.push_item(entry_key);
            keypath
        }
    }

    mod deserialize_impl {
        use super::Map;
        use crate::storage::{Keypath, KeypathSeed, Load};
        use core::fmt;
        use core::marker::PhantomData;
        use serde::de::{DeserializeSeed, Visitor};
        use serde::Deserializer;

        impl<'de, K, V> DeserializeSeed<'de> for KeypathSeed<Map<K, V>> {
            type Value = Map<K, V>;

            fn deserialize<D>(
                self,
                deserializer: D,
            ) -> Result<Self::Value, D::Error>
            where
                D: Deserializer<'de>,
            {
                struct KeypathVisitor<'a, K, V> {
                    keypath: &'a mut Keypath,
                    marker: PhantomData<(K, V)>,
                }

                impl<'de, 'a, K, V> Visitor<'de> for KeypathVisitor<'a, K, V> {
                    type Value = Map<K, V>;

                    fn expecting(
                        &self,
                        formatter: &mut fmt::Formatter,
                    ) -> fmt::Result {
                        write!(formatter, "a Map")
                    }

                    fn visit_unit<E>(self) -> Result<Self::Value, E> {
                        Ok(Map::new(self.keypath))
                    }
                }

                let mut keypath = self.keypath.clone();
                let visitor = KeypathVisitor {
                    keypath: &mut keypath,
                    marker: PhantomData,
                };
                deserializer.deserialize_unit(visitor)
            }
        }

        impl<K, V> Load for Map<K, V> {
            fn load_keypath(keypath: &Keypath) -> crate::result::Result<Self> {
                Ok(From::from(keypath.clone()))
            }
        }
    }
}
