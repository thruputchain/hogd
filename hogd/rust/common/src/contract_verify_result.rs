/// FFI-safe result of contract verification.
#[repr(C)]
pub struct ContractVerifyResult {
    pub success: bool,
}

impl ContractVerifyResult {
    pub fn ok() -> Self {
        Self { success: true }
    }

    pub fn err(msg: String) -> Self {
        #[cfg(debug_assertions)]
        eprintln!("Contract verification failed: {}", msg);
        let _ = msg;
        Self { success: false }
    }
}
