pub use hogexec_internal::ffi_enums::*;

pub mod contract_loader;

mod trampoline_bindings;
pub use trampoline_bindings::*;

use core::slice;
use core::str::Utf8Error;

type ForeignHostCallContext = *mut core::ffi::c_void;

/// An opaque, generic function pointer type used as a carrier for
/// C function pointers received via FFI. Using a fn pointer (rather than
/// `*mut c_void`) avoids undefined behavior when transmuting to a
/// concrete function pointer type, since fn-ptr-to-fn-ptr transmute
/// is well-defined in Rust.
pub type ErasedFnPtr = unsafe extern "C" fn();

/// # Safety
/// `bytes` must be a valid pointer to `len` bytes.
pub unsafe fn string_from_parts(
    bytes: *const u8,
    len: u32,
) -> Result<String, Utf8Error> {
    let slice = unsafe { slice::from_raw_parts(bytes, len as usize) };
    std::str::from_utf8(slice).map(|s| s.to_owned())
}

/// A wrapper for a C function pointer that is safe to send across threads.
///
/// # Safety
/// The contained function pointer must remain valid for the lifetime of this value.
#[derive(Clone, Copy)]
#[repr(transparent)]
pub struct SendFnPtr(ErasedFnPtr);

unsafe impl Send for SendFnPtr {}
unsafe impl Sync for SendFnPtr {}

impl SendFnPtr {
    pub fn new(ptr: ErasedFnPtr) -> Self {
        Self(ptr)
    }
    pub fn get(self) -> ErasedFnPtr {
        self.0
    }
}

/// An entry describing a host function to be linked into a WASM module.
#[derive(Clone)]
pub struct LinkEntry {
    module: String,
    function: String,
    nargs: u8,
    fn_pointer: SendFnPtr,
}

impl LinkEntry {
    pub fn new(
        module: String,
        function: String,
        nargs: u8,
        fn_pointer: ErasedFnPtr,
    ) -> Self {
        Self {
            module,
            function,
            nargs,
            fn_pointer: SendFnPtr::new(fn_pointer),
        }
    }

    pub fn module(&self) -> &str {
        &self.module
    }

    pub fn function(&self) -> &str {
        &self.function
    }

    pub fn nargs(&self) -> u8 {
        self.nargs
    }

    pub fn fn_pointer(&self) -> ErasedFnPtr {
        self.fn_pointer.get()
    }
}
