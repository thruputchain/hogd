use crate::ForeignHostCallContext;
/// Manages fetching contract data across the FFI boundary
///
/// Warning: this struct returns pointers to external data (i.e. data held within the
/// WasmiContext on the C++ side).
/// As such, this struct cannot outlive the WasmiContext.)
use crate::SystemErrorCode;
use core::slice;

#[repr(C)]
pub struct ContractGetResult {
    err_code: i32, // SystemErrorCode
    data: *const u8,
    data_len: usize,
}

impl ContractGetResult {
    pub fn new(err_code: i32, data: &'static [u8]) -> Self {
        Self {
            err_code,
            data: data.as_ptr(),
            data_len: data.len(),
        }
    }
}

pub type ContractGetFn = unsafe extern "C" fn(
    ForeignHostCallContext,
    *const u8,
) -> ContractGetResult;

// Lifetime is be lifetime of the block, not static.
impl ContractGetResult {
    pub fn to_data(&self) -> Result<Option<&'static [u8]>, SystemErrorCode> {
        if self.err_code != 0 {
            return match self.err_code.try_into() {
                Ok(err) => Err(err),
                Err(_) => Err(SystemErrorCode::kSystemUnrecoverable),
            };
        }
        if self.data.is_null() {
            return Ok(None);
        }
        Ok(Some(unsafe {
            slice::from_raw_parts(self.data, self.data_len)
        }))
    }
}
