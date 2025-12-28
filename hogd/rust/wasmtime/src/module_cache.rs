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
    hash: *const u8,
    hash_len: usize,
) -> CompiledCodeGetResult;

pub type CompiledCodePutFn = unsafe extern "C" fn(
    hash: *const u8,
    hash_len: usize,
    data: *const u8,
    data_len: usize,
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

// Module cache is NOT threadsafe, and should NOT be shared across threads.
pub struct ModuleCache {
    compiled_code_get_fn: CompiledCodeGetFn,
    compiled_code_put_fn: CompiledCodePutFn,
}

impl ModuleCache {
    pub fn new(
        compiled_code_get_fn: CompiledCodeGetFn,
        compiled_code_put_fn: CompiledCodePutFn,
    ) -> Self {
        Self {
            compiled_code_get_fn,
            compiled_code_put_fn,
        }
    }

    pub fn get(
        &mut self,
        hash: &SHA256Hash,
        engine: &Engine,
    ) -> Result<Option<Module>, SystemErrorCode> {
        let result =
            unsafe { (self.compiled_code_get_fn)(hash.as_ptr(), hash.len()) };
        let slice = match result.as_slice() {
            Some(slice) => slice,
            None => return Ok(None),
        };
        // The deserialize_raw API does not work on environments that use virtual memory.
        // TODO(geoff): It would be really nice if we could avoid doing a full copy of the module,
        // followed by an mprotect.
        //
        //let module = unsafe { Module::deserialize_raw(engine, slice.into()) }
        let module = unsafe { Module::deserialize(engine, slice) }
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
                hash.len(),
                bytes.as_ptr(),
                bytes.len(),
            )
        };
        Ok(())
    }
}
