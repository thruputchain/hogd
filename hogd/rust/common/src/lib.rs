pub use hogexec_internal::ffi_enums::*;

pub mod contract_loader;

mod trampoline_bindings;
pub use trampoline_bindings::*;

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
