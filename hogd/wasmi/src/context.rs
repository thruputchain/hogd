use wasmi::{Caller, Config, Engine, Error, Linker, StackLimits};

use core::ffi::c_void;
use core::slice;
use core::str::Utf8Error;

use crate::external_call;

// A WasmiContext provides shared data structures
// for multiple Wasmi runtimes.
// This structure internally (appears to be) threadsafe,
// although our usage pattern should allocate one per
// each transaction.
//
// This borrows the wasmi use patterns from stellar:
// e.g. https://github.com/stellar/rs-soroban-env/blob/f9a51971d3ca66216e290b074a1feee5334b58ff/soroban-env-host/src/vm.rs#L166
//
// Engine can be potentially shared between instances.
// The WASM official C API spec says that only one ``engine'' should be created
// per process, although that's more strict than necessary.
// Linker can be shared between instances.
// Store and Module and Instance are per-WasmiRuntime structures.

pub struct WasmiContext {
    pub engine: Engine,
    pub linker: Linker<*mut c_void>,
}

// Tells rust that it is safe to send this c_void pointer
// across lambda boundaries
#[derive(Clone)]
struct BorrowBypass {
    fn_pointer: *mut c_void,
}

unsafe impl Send for BorrowBypass {}
unsafe impl Sync for BorrowBypass {}

fn string_from_parts(bytes: *const u8, len: u32) -> Result<String, Utf8Error> {
    let slice = unsafe { slice::from_raw_parts(bytes, len as usize) };
    match std::str::from_utf8(&slice) {
        Ok(s) => Ok(s.to_owned()), // TODO check this is a copy and not something weird
        Err(x) => Err(x),
    }
}

impl WasmiContext {
    fn new() -> WasmiContext {
        let stack_limit = StackLimits::default();

        let engine = Engine::new(
            &Config::default()
                .consume_fuel(true)
                .floats(false)
                .set_stack_limits(stack_limit),
        );

        Self {
            engine: engine.clone(),
            linker: Linker::new(&engine),
        }
    }

    fn link_function_0args(
        &mut self,
        fn_pointer: *mut c_void,
        import_name: &str,
        fn_name: &str,
    ) -> Result<(), Error> {
        let x = BorrowBypass {
            fn_pointer: fn_pointer.clone(),
        };

        match self.linker.func_wrap(
            import_name,
            fn_name,
            move |caller: Caller<'_, *mut c_void>| -> Result<u64, wasmi::Error> {

                let res = unsafe {
                    external_call::c_call_0args(x.clone().fn_pointer, caller.data().clone())
                };

                return external_call::handle_trampoline_error(res);
        }) {
            Ok(_) => Ok(()),
            Err(err) => Err(err.into()),
        }
    }

    fn link_function_1args(
        &mut self,
        fn_pointer: *mut c_void,
        import_name: &str,
        fn_name: &str,
    ) -> Result<(), Error> {
        let x = BorrowBypass {
            fn_pointer: fn_pointer.clone(),
        };

        match self.linker.func_wrap(
            import_name,
            fn_name,
            move |caller: Caller<'_, *mut c_void>,
                  arg1: u64|
                  -> Result<u64, wasmi::Error> {
                let res = unsafe {
                    external_call::c_call_1args(
                        x.clone().fn_pointer,
                        caller.data().clone(),
                        arg1,
                    )
                };

                return external_call::handle_trampoline_error(res);
            },
        ) {
            Ok(_) => Ok(()),
            Err(err) => Err(err.into()),
        }
    }

    fn link_function_2args(
        &mut self,
        fn_pointer: *mut c_void,
        import_name: &str,
        fn_name: &str,
    ) -> Result<(), Error> {
        let x = BorrowBypass {
            fn_pointer: fn_pointer.clone(),
        };

        match self.linker.func_wrap(
            import_name,
            fn_name,
            move |caller: Caller<'_, *mut c_void>,
                  arg1: u64,
                  arg2: u64|
                  -> Result<u64, wasmi::Error> {
                let res = unsafe {
                    external_call::c_call_2args(
                        x.clone().fn_pointer,
                        caller.data().clone(),
                        arg1,
                        arg2,
                    )
                };
                return external_call::handle_trampoline_error(res);
            },
        ) {
            Ok(_) => Ok(()),
            Err(err) => Err(err.into()),
        }
    }

    fn link_function_3args(
        &mut self,
        fn_pointer: *mut c_void,
        import_name: &str,
        fn_name: &str,
    ) -> Result<(), Error> {
        let x = BorrowBypass {
            fn_pointer: fn_pointer.clone(),
        };

        match self.linker.func_wrap(
            import_name,
            fn_name,
            move |caller: Caller<'_, *mut c_void>,
                  arg1: u64,
                  arg2: u64,
                  arg3: u64|
                  -> Result<u64, wasmi::Error> {
                let res = unsafe {
                    external_call::c_call_3args(
                        x.clone().fn_pointer,
                        caller.data().clone(),
                        arg1,
                        arg2,
                        arg3,
                    )
                };
                return external_call::handle_trampoline_error(res);
            },
        ) {
            Ok(_) => Ok(()),
            Err(err) => Err(err.into()),
        }
    }

    fn link_function_4args(
        &mut self,
        fn_pointer: *mut c_void,
        import_name: &str,
        fn_name: &str,
    ) -> Result<(), Error> {
        let x = BorrowBypass {
            fn_pointer: fn_pointer.clone(),
        };

        match self.linker.func_wrap(
            import_name,
            fn_name,
            move |caller: Caller<'_, *mut c_void>,
                  arg1: u64,
                  arg2: u64,
                  arg3: u64,
                  arg4: u64|
                  -> Result<u64, wasmi::Error> {
                let res = unsafe {
                    external_call::c_call_4args(
                        x.clone().fn_pointer,
                        caller.data().clone(),
                        arg1,
                        arg2,
                        arg3,
                        arg4,
                    )
                };
                return external_call::handle_trampoline_error(res);
            },
        ) {
            Ok(_) => Ok(()),
            Err(err) => Err(err.into()),
        }
    }

    fn link_function_5args(
        &mut self,
        fn_pointer: *mut c_void,
        import_name: &str,
        fn_name: &str,
    ) -> Result<(), Error> {
        let x = BorrowBypass {
            fn_pointer: fn_pointer.clone(),
        };

        match self.linker.func_wrap(
            import_name,
            fn_name,
            move |caller: Caller<'_, *mut c_void>,
                  arg1: u64,
                  arg2: u64,
                  arg3: u64,
                  arg4: u64,
                  arg5: u64|
                  -> Result<u64, wasmi::Error> {
                let res = unsafe {
                    external_call::c_call_5args(
                        x.clone().fn_pointer,
                        caller.data().clone(),
                        arg1,
                        arg2,
                        arg3,
                        arg4,
                        arg5,
                    )
                };
                return external_call::handle_trampoline_error(res);
            },
        ) {
            Ok(_) => Ok(()),
            Err(err) => Err(err.into()),
        }
    }
}

#[no_mangle]
pub extern "C" fn wasmi_link_nargs(
    context_void: *mut c_void,
    module_name: *const u8,
    module_name_len: u32,
    method_name: *const u8,
    method_name_len: u32,
    function_pointer: *mut c_void,
    nargs: u8,
) -> bool // true if success
{
    let context: *mut WasmiContext =
        unsafe { core::mem::transmute(context_void) };

    assert!(context != core::ptr::null_mut());

    assert!(module_name != core::ptr::null());
    assert!(method_name != core::ptr::null());

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

    let c = unsafe { &mut *context };

    let res = match nargs {
        0 => c.link_function_0args(function_pointer, &module, &method),
        1 => c.link_function_1args(function_pointer, &module, &method),
        2 => c.link_function_2args(function_pointer, &module, &method),
        3 => c.link_function_3args(function_pointer, &module, &method),
        4 => c.link_function_4args(function_pointer, &module, &method),
        5 => c.link_function_5args(function_pointer, &module, &method),
        _ => {
            return false;
        }
    };

    match res {
        Ok(_) => true,
        Err(_) => false,
    }
}

// Rust FFI needs no_mangle and extern "C"
#[no_mangle]
pub extern "C" fn new_wasmi_context() -> *mut c_void {
    let b = Box::new(WasmiContext::new());

    return unsafe { core::mem::transmute(Box::into_raw(b)) };
}

#[no_mangle]
pub extern "C" fn free_wasmi_context(p: *mut c_void) {
    assert!(p != core::ptr::null_mut());

    let runtime: *mut WasmiContext = unsafe { core::mem::transmute(p) };

    unsafe { drop(Box::from_raw(runtime)) };
}
