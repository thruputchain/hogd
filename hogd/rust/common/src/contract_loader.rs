use crate::ForeignHostCallContext;
use core::slice;
/// Manages fetching contract data across the FFI boundary
///
/// Warning: this struct returns pointers to external data (i.e. data held within the
/// WasmiContext on the C++ side).
/// As such, this struct cannot outlive the WasmiContext.)
use hogexec_internal::ffi_enums::SystemErrorCode;

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

impl ContractGetResult {
    /// # Safety
    /// The returned slice's lifetime `'a` is unconstrained by the type system.
    /// The data is owned by the C++ storage snapshot cache, which is only recycled
    /// on block boundaries. Callers must ensure the returned slice does not outlive
    /// the current transaction scope.
    pub unsafe fn to_data<'a>(
        &self,
    ) -> Result<Option<&'a [u8]>, SystemErrorCode> {
        if self.err_code != 0 {
            return match self.err_code.try_into() {
                Ok(err) => Err(err),
                Err(_) => Err(SystemErrorCode::Unrecoverable),
            };
        }
        if self.data.is_null() || self.data_len == 0 {
            return Ok(None);
        }
        Ok(Some(unsafe {
            slice::from_raw_parts(self.data, self.data_len)
        }))
    }
}
