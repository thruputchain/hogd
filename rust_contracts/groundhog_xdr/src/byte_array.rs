/// A wrapper type that allows arrays of arbitrary size
/// to be serialized with serde
use serde::{Deserialize, Serialize};

#[derive(Serialize, Deserialize, Debug, PartialEq, Eq, Copy, Clone)]
pub struct ByteArray<const N: usize> {
    #[serde(with = "serde_arrays")]
    arr: [u8; N],
}

impl<const N: usize> Default for ByteArray<N> {
    fn default() -> Self {
        Self { arr: [0; N] }
    }
}

impl<const N: usize> ByteArray<N> {
    pub const fn new(arr: [u8; N]) -> Self {
        Self { arr }
    }

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

    pub const fn as_ptr(&self) -> *const u8 {
        self.arr.as_ptr()
    }

    pub const fn len(&self) -> usize {
        N
    }
}
