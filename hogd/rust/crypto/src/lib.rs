pub mod ed25519;
pub mod siphash24;

#[repr(C)]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct MessageSpan {
    pub message_ptr: *const u8,
    pub message_len: usize,
}

impl MessageSpan {
    /// # Safety
    /// `message_ptr` must point to valid memory of at least `message_len` bytes,
    /// and the memory must remain valid for the lifetime of the returned slice.
    pub unsafe fn to_slice(&self) -> &[u8] {
        if self.message_len == 0 {
            return &[];
        }
        core::slice::from_raw_parts(self.message_ptr, self.message_len)
    }
}
