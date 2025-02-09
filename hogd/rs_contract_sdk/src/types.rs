//! Types

pub use ::rs_xdr::types::*;



/// A wrapper to get serialization right
pub type Ed25519SignatureA = byte_array::ByteArray<64>;

/// Same as `Signature`, but using the wrapped subvalue
#[derive(Debug, PartialEq, Eq, serde::Serialize, serde::Deserialize)]
#[repr(i32)]
pub enum SignatureA {
    Ed25519 {
        ed25519: Ed25519SignatureA
    } = CryptographicScheme::Ed25519 as i32,
}

/// A wrapper type that allows arrays of arbitrary size
/// to be serialized with serde
mod byte_array {
    use serde::{Serialize, Deserialize};

    #[derive(Serialize, Deserialize, Debug, PartialEq, Eq)]
    pub struct ByteArray<const N: usize> {
        #[serde(with = "serde_arrays")]
        arr: [u8; N],
    }

    impl<const N: usize> ByteArray<N> {
        pub fn as_slice(&self) -> &[u8] {
            &self.arr
        }

        pub fn as_mut(&mut self) -> &mut [u8] {
            &mut self.arr
        }

        pub fn as_array(&self) -> &[u8; N] {
            &self.arr
        }

        pub fn into_inner(self) -> [u8; N] {
            self.arr
        }
    }
}
