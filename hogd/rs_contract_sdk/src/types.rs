pub use rs_xdr::*;
use serde::{Serialize, Deserialize};

#[derive(Serialize, Deserialize, Debug, PartialEq, Eq)]
pub struct FixedBytes<const N: usize> {
    #[serde(with = "serde_arrays")]
    arr: [u8; N],
}

impl<const N: usize> FixedBytes<N> {
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

pub type Ed25519SignatureA = FixedBytes<64>;

#[derive(Serialize, Deserialize, Debug, PartialEq, Eq)]
#[repr(i32)]
pub enum SignatureA {
    Ed25519 {
        ed25519: Ed25519SignatureA
    } = CryptographicScheme::Ed25519 as i32,
}
