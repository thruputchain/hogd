pub use hogexec_internal::ffi_enums::*;

pub mod contract_loader;

mod trampoline_bindings;
pub use trampoline_bindings::*;

use core::slice;
use core::str::Utf8Error;

pub const MAX_TABLE_ELEMENTS: u64 = 64;

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

/// An entry describing a host function to be linked into a WASM module.
#[derive(Clone)]
pub struct LinkEntry {
    module: String,
    function: String,
    nargs: u8,
    fn_pointer: ErasedFnPtr,
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
            fn_pointer,
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
        self.fn_pointer
    }
}

pub fn verify_contract_meter(
    contract_data: &[u8],
    host_imports: &[LinkEntry],
) -> InvokeErrorCode {
    let config = groundhog_wasm_meter::Config {
        max_imported_functions: 1,
        max_table_initial_size: MAX_TABLE_ELEMENTS,
        reject_floats: true,
        allowed_custom_sections: vec![
            "name".to_string(),
            "groundhogspecv0".to_string(),
        ],
        ..groundhog_wasm_meter::Config::default()
    };
    let module_info = match groundhog_wasm_meter::verify(contract_data, &config)
    {
        Ok(info) => info,
        Err(e) => {
            #[cfg(debug_assertions)]
            eprintln!("Contract verification failed: wasm-meter: {e}");
            return match e {
                groundhog_wasm_meter::Error::InternalResourceExhausted(_) => {
                    InvokeErrorCode::Unrecoverable
                }
                _ => InvokeErrorCode::Deterministic,
            };
        }
    };
    if module_info.memories.len() != 1 {
        #[cfg(debug_assertions)]
        eprintln!(
            "Contract verification failed: expected 1 memory, found {}",
            module_info.memories.len()
        );
        return InvokeErrorCode::Deterministic;
    }
    if module_info.memories[0].initial_pages > 1 {
        #[cfg(debug_assertions)]
        eprintln!(
            "Contract verification failed: initial memory {} pages exceeds limit of 1",
            module_info.memories[0].initial_pages
        );
        return InvokeErrorCode::Deterministic;
    }
    if module_info.memories[0].export_name != Some("memory") {
        #[cfg(debug_assertions)]
        eprintln!(
            "Contract verification failed: memory not exported as \"memory\""
        );
        return InvokeErrorCode::Deterministic;
    }
    for import in &module_info.imports {
        let entry = host_imports.iter().find(|e| {
            e.module() == import.module && e.function() == import.name
        });
        match entry {
            Some(e) => {
                if import.n_params != e.nargs() as usize {
                    eprintln!(
                        "rejecting contract — import {}::{} arity mismatch: contract declares {} params, host registers {} (likely SDK/runtime ABI mismatch)",
                        import.module,
                        import.name,
                        import.n_params,
                        e.nargs()
                    );
                    return InvokeErrorCode::Deterministic;
                }
            }
            None => {
                eprintln!(
                    "rejecting contract — unknown host import {}::{} (likely SDK/runtime ABI mismatch)",
                    import.module,
                    import.name
                );
                return InvokeErrorCode::Deterministic;
            }
        }
    }
    InvokeErrorCode::None
}
