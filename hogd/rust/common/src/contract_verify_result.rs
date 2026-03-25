/// Debug-only error detail. Carries the validation error message.
/// Must be freed with free_contract_verify_error_detail.
#[repr(C)]
pub struct ContractVerifyErrorDetail {
    pub error_msg: *const u8,
    pub error_msg_len: u32,
}

/// FFI-safe result of contract verification.
/// - success: true if valid, false if invalid
/// - detail: null in release builds (zero overhead).
///   In debug builds, non-null on failure — points to heap-allocated error detail.
///   Must be freed with free_contract_verify_error_detail if non-null.
#[repr(C)]
pub struct ContractVerifyResult {
    pub success: bool,
    pub detail: *mut ContractVerifyErrorDetail,
}

impl ContractVerifyResult {
    pub fn ok() -> Self {
        Self {
            success: true,
            detail: core::ptr::null_mut(),
        }
    }

    pub fn err(msg: String) -> Self {
        #[cfg(debug_assertions)]
        {
            let bytes = msg.into_bytes().into_boxed_slice();
            let len = bytes.len() as u32;
            let msg_ptr = Box::into_raw(bytes) as *const u8;
            let detail = Box::into_raw(Box::new(ContractVerifyErrorDetail {
                error_msg: msg_ptr,
                error_msg_len: len,
            }));
            Self {
                success: false,
                detail,
            }
        }
        #[cfg(not(debug_assertions))]
        {
            let _ = msg;
            Self {
                success: false,
                detail: core::ptr::null_mut(),
            }
        }
    }
}

/// Frees a ContractVerifyErrorDetail allocated by ContractVerifyResult::err().
/// No-op if detail is null.
///
/// # Safety
/// detail must be null or a pointer returned by a ContractVerifyResult.
/// Must only be called once per detail pointer.
#[no_mangle]
pub unsafe extern "C" fn free_contract_verify_error_detail(
    detail: *mut ContractVerifyErrorDetail,
) {
    if detail.is_null() {
        return;
    }
    let detail = unsafe { Box::from_raw(detail) };
    if !detail.error_msg.is_null() && detail.error_msg_len > 0 {
        let ptr = detail.error_msg as *mut u8;
        let len = detail.error_msg_len as usize;
        unsafe {
            drop(Box::from_raw(core::slice::from_raw_parts_mut(ptr, len)));
        }
    }
}
