#![no_std]

pub mod ed25519;
pub mod siphash24;

#[repr(C)]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct MessageSpan {
    pub message_ptr: *const u8,
    pub message_len: usize,
}

impl MessageSpan {
    pub fn to_slice(&self) -> &[u8] {
        unsafe {
            core::slice::from_raw_parts(self.message_ptr, self.message_len)
        }
    }
}
