pub use hogexec_internal::ffi_enums::*;

pub mod contract_loader;

mod trampoline_bindings;
pub use trampoline_bindings::*;

use core::ffi::c_void;
use core::slice;
use core::str::Utf8Error;

type ForeignHostCallContext = *mut core::ffi::c_void;

pub fn string_from_parts(
    bytes: *const u8,
    len: u32,
) -> Result<String, Utf8Error> {
    let slice = unsafe { slice::from_raw_parts(bytes, len as usize) };
    std::str::from_utf8(slice).map(|s| s.to_owned())
}

/// A raw pointer wrapper that is safe to send across threads.
///
/// # Safety
/// The pointer must point to thread-safe data (e.g., a static function pointer).
#[derive(Clone, Copy)]
#[repr(transparent)]
pub struct SendPtr(*mut c_void);

unsafe impl Send for SendPtr {}
unsafe impl Sync for SendPtr {}

impl SendPtr {
    pub const fn new(ptr: *mut c_void) -> Self {
        Self(ptr)
    }
    pub fn as_ptr(self) -> *mut c_void {
        self.0
    }
}

/// An entry describing a host function to be linked into a WASM module.
#[derive(Clone)]
pub struct LinkEntry {
    module: String,
    function: String,
    nargs: u8,
    fn_pointer: SendPtr,
}

impl LinkEntry {
    pub fn new(
        module: String,
        function: String,
        nargs: u8,
        fn_pointer: *mut c_void,
    ) -> Self {
        Self {
            module,
            function,
            nargs,
            fn_pointer: SendPtr::new(fn_pointer),
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

    pub fn fn_pointer(&self) -> *mut c_void {
        self.fn_pointer.as_ptr()
    }
}
