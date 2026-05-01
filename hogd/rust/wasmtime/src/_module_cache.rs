use wasmtime::Engine;
use wasmtime::Module;

use crate::debug_log;
use crate::SHA256Hash;
use common::SystemErrorCode;

#[repr(C)]
pub struct CompiledCodeGetResult {
    pub data: *const u8,
    pub data_len: usize,
}

// The lifetime of the returned compiled code is at least until the end of the block (hence until the end of the transaction).
// So it is safe to use the returned slices for the duration of a running transaction,
// although we will need more work to be done if we want to copy some modules into a threadlocal InstancePre cache.
pub type CompiledCodeGetFn = unsafe extern "C" fn(
    hash: *const u8, // 32 bytes
    external_cache: *mut core::ffi::c_void,
) -> CompiledCodeGetResult;

pub type CompiledCodePutFn = unsafe extern "C" fn(
    hash: *const u8, // 32 bytes
    data: *const u8,
    data_len: usize,
    external_cache: *mut core::ffi::c_void,
);

impl CompiledCodeGetResult {
    pub fn as_slice<'a>(&self) -> Option<&'a [u8]> {
        if self.data.is_null() {
            None
        } else {
            Some(unsafe {
                core::slice::from_raw_parts(self.data, self.data_len)
            })
        }
    }
}

pub struct ModuleCache {
    compiled_code_get_fn: CompiledCodeGetFn,
    compiled_code_put_fn: CompiledCodePutFn,
    external_cache: *mut core::ffi::c_void,
}

impl ModuleCache {
    pub fn new(
        compiled_code_get_fn: CompiledCodeGetFn,
        compiled_code_put_fn: CompiledCodePutFn,
        external_cache: *mut core::ffi::c_void,
    ) -> Self {
        Self {
            compiled_code_get_fn,
            compiled_code_put_fn,
            external_cache,
        }
    }

    pub fn get(
        &mut self,
        hash: &SHA256Hash,
        engine: &Engine,
    ) -> Result<Option<Module>, SystemErrorCode> {
        let result = unsafe {
            (self.compiled_code_get_fn)(hash.as_ptr(), self.external_cache)
        };
        let slice = match result.as_slice() {
            Some(slice) => slice,
            None => return Ok(None),
        };

        let module =  unsafe { Module::deserialize(engine, slice) }
            .map_err(|_e| {
                debug_log!(eprintln!("Module deserialize error: {_e}"));
                SystemErrorCode::kSystemUnrecoverable
            })?;
        Ok(Some(module))
    }

    pub fn put(
        &mut self,
        hash: &SHA256Hash,
        module: &Module,
    ) -> Result<(), SystemErrorCode> {
        let bytes = module.serialize().map_err(|_e| {
            debug_log!(eprintln!("Module serialize error: {_e}"));
            SystemErrorCode::kSystemUnrecoverable
        })?;
        unsafe {
            (self.compiled_code_put_fn)(
                hash.as_ptr(),
                bytes.as_ptr(),
                bytes.len(),
                self.external_cache,
            )
        };
        Ok(())
    }
}
