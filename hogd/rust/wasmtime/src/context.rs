use core::ffi::c_void;
use core::slice;

use common::string_from_parts;

use wasmtime::{Caller, Config, Engine, Linker, Module};

use crate::error::TrampolineError;
use common::TrampolineResult;

pub struct LinkEntry {
    module: String,
    function: String,
    nargs: u8,
    fn_pointer: *mut c_void,
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
            fn_pointer,
        }
    }
}

pub struct WasmtimeContext {
    pub entries: Vec<LinkEntry>,
}

// Tells rust that it is safe to send this c_void pointer
// across lambda boundaries
#[derive(Clone)]
struct BorrowBypass {
    fn_pointer: *mut c_void,
}

unsafe impl Send for BorrowBypass {}
unsafe impl Sync for BorrowBypass {}

fn wasmtime_handle_trampoline_error(
    result: TrampolineResult,
) -> Result<i32, wasmtime::Error> {
    let error = result.panic.try_into();
    if error.is_err() {
        return Err(wasmtime::Error::new(TrampolineError {
            error: common::HostFnErrorCode::kHostUnrecoverable,
        }));
    }
    let error = error.unwrap();
    match error {
        common::HostFnErrorCode::kHostNoneOrRecoverable => Ok(result.result),
        _ => Err(wasmtime::Error::new(TrampolineError { error })),
    }
}

macro_rules! link_function_nargs {
    ($name:ident, $cfunc:ident $(, $arg:ident)*) => {
        fn $name(
            linker: &mut Linker<*mut c_void>,
            fn_pointer: *mut c_void,
            import_name: &str,
            fn_name: &str,
        ) -> Result<(), wasmtime::Error> {
            let x = BorrowBypass { fn_pointer };
            linker
                .func_wrap(import_name, fn_name, move |caller: Caller<'_, *mut c_void>, $($arg: u64),*| {
                    let res = unsafe {
                        common::$cfunc(
                            x.clone().fn_pointer,
                            *caller.data(),
                            $($arg),*
                        )
                    };
                    wasmtime_handle_trampoline_error(res)
                })
                .map_err(Into::into)
                .map(|_| ())
        }
    };
}

impl WasmtimeContext {
    fn new() -> WasmtimeContext {
        Self {
            entries: Vec::new(),
        }
    }

    pub fn add_entry(&mut self, entry: LinkEntry) {
        self.entries.push(entry);
    }

    fn new_engine() -> Engine {
        Engine::new(
            &Config::default()
                .consume_fuel(true)
                .wasm_backtrace(false)
                .strategy(wasmtime::Strategy::Winch)
                .cranelift_opt_level(wasmtime::OptLevel::Speed)
                .cranelift_nan_canonicalization(true) // deterministic nan
                .wasm_backtrace(false)
                .epoch_interruption(false)
                .max_wasm_stack(16 * 1024)
        )
        .unwrap()
    }

    pub fn linker(&self) -> Linker<*mut c_void> {
        let engine = Self::new_engine();
        let mut linker = Linker::new(&engine);

        for entry in &self.entries {
            match entry.nargs {
                0 => Self::link_function_0args(
                    &mut linker,
                    entry.fn_pointer,
                    &entry.module,
                    &entry.function,
                ),
                1 => Self::link_function_1args(
                    &mut linker,
                    entry.fn_pointer,
                    &entry.module,
                    &entry.function,
                ),
                2 => Self::link_function_2args(
                    &mut linker,
                    entry.fn_pointer,
                    &entry.module,
                    &entry.function,
                ),
                3 => Self::link_function_3args(
                    &mut linker,
                    entry.fn_pointer,
                    &entry.module,
                    &entry.function,
                ),
                4 => Self::link_function_4args(
                    &mut linker,
                    entry.fn_pointer,
                    &entry.module,
                    &entry.function,
                ),
                5 => Self::link_function_5args(
                    &mut linker,
                    entry.fn_pointer,
                    &entry.module,
                    &entry.function,
                ),
                6 => Self::link_function_6args(
                    &mut linker,
                    entry.fn_pointer,
                    &entry.module,
                    &entry.function,
                ),
                7 => Self::link_function_7args(
                    &mut linker,
                    entry.fn_pointer,
                    &entry.module,
                    &entry.function,
                ),
                8 => Self::link_function_8args(
                    &mut linker,
                    entry.fn_pointer,
                    &entry.module,
                    &entry.function,
                ),
                _ => {
                    panic!("Unsupported number of arguments: {}", entry.nargs);
                }
            }
            .unwrap();
        }

        linker
    }

    fn verify_contract(&self, contract_data: &[u8]) -> bool {
        // TODO(geoff): consider caching the engine
        let engine = Self::new_engine();
        Module::validate(&engine, contract_data).is_ok()
    }

    link_function_nargs!(link_function_0args, c_call_0args);
    link_function_nargs!(link_function_1args, c_call_1args, arg1);
    link_function_nargs!(link_function_2args, c_call_2args, arg1, arg2);
    link_function_nargs!(link_function_3args, c_call_3args, arg1, arg2, arg3);
    link_function_nargs!(
        link_function_4args,
        c_call_4args,
        arg1,
        arg2,
        arg3,
        arg4
    );
    link_function_nargs!(
        link_function_5args,
        c_call_5args,
        arg1,
        arg2,
        arg3,
        arg4,
        arg5
    );
    link_function_nargs!(
        link_function_6args,
        c_call_6args,
        arg1,
        arg2,
        arg3,
        arg4,
        arg5,
        arg6
    );
    link_function_nargs!(
        link_function_7args,
        c_call_7args,
        arg1,
        arg2,
        arg3,
        arg4,
        arg5,
        arg6,
        arg7
    );
    link_function_nargs!(
        link_function_8args,
        c_call_8args,
        arg1,
        arg2,
        arg3,
        arg4,
        arg5,
        arg6,
        arg7,
        arg8
    );
}

/// # Safety
/// Caller must have exclusive (non-concurrent) access to the context, and context must not be a nullptr.
/// Context must have been created by new_wasmi_context.
#[no_mangle]
pub unsafe extern "C" fn wasmtime_link_nargs(
    context_void: *mut c_void,
    module_name: *const u8,
    module_name_len: u32,
    method_name: *const u8,
    method_name_len: u32,
    function_pointer: *mut c_void,
    nargs: u8,
) -> bool // true if success
{
    let context: *mut WasmtimeContext =
        unsafe { core::mem::transmute(context_void) };

    debug_assert!(!context.is_null());
    debug_assert!(!module_name.is_null());
    debug_assert!(!method_name.is_null());

    // assert module name & method_name nonzero
    // WASMI (and apparently the wasm standard) will accept modules with no names,
    let module = match string_from_parts(module_name, module_name_len) {
        Ok(x) => x,
        _ => {
            return false;
        }
    };

    let method = match string_from_parts(method_name, method_name_len) {
        Ok(x) => x,
        _ => {
            return false;
        }
    };

    debug_assert!(!module.is_empty());
    debug_assert!(!method.is_empty());

    let c = unsafe { &mut *context };

    c.add_entry(LinkEntry::new(module, method, nargs, function_pointer));
    true
}

// Rust FFI needs no_mangle and extern "C"
/// # Safety
/// module_cache can be nullptr, in which case no module cache is used.
/// module_cache is threadsafe, so it can be safely shared across threads.
/// module_cache must outlive WasmiContext.
#[no_mangle]
pub unsafe extern "C" fn new_wasmtime_context() -> *mut c_void {
    let b = Box::new(WasmtimeContext::new());

    unsafe { core::mem::transmute(Box::into_raw(b)) }
}

/// # Safety
/// Caller must have exclusive (non-concurrent) access to the context, and context must not be a nullptr.
/// Context must have been created by new_wasmi_context.
/// Context must not be accessed after this call.
#[no_mangle]
pub unsafe extern "C" fn free_wasmtime_context(p: *mut c_void) {
    debug_assert!(!p.is_null());

    let context: *mut WasmtimeContext = unsafe { core::mem::transmute(p) };

    unsafe { drop(Box::from_raw(context)) };
}

/// # Safety
/// Context must not be a nullptr.
/// Context must have been created by new_wasmi_context.
/// Multiple contracts can be verified concurrently.
/// The linker can be modified concurrently.
#[no_mangle]
pub unsafe extern "C" fn wasmtime_verify_contract(
    p: *const c_void,
    contract_data_p: *const u8,
    contract_size: usize,
) -> bool {
    debug_assert!(!p.is_null());

    // empty contract might trigger this case.
    if contract_data_p.is_null() {
        return false;
    }

    let context: *const WasmtimeContext = unsafe { core::mem::transmute(p) };

    let wasm: &[u8] =
        unsafe { slice::from_raw_parts(contract_data_p, contract_size) };

    unsafe { (*context).verify_contract(wasm) }
}
