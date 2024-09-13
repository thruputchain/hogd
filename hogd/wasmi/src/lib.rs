use wasmi::{
    Caller, Config, Engine, Error, Func, Instance, Linker, Module, StackLimits,
    Store, Val,
};

use core::ffi::c_void;
use std::slice;

use core::str::Utf8Error;

mod external_call;
mod invoke_result;

pub use crate::invoke_result::{WasmiInvokeResult, WasmiInvokeError};

pub struct WasmiRuntime {
    store: Store<*mut c_void>,
    module: Module,
    linker: Linker<*mut c_void>,
    instance: Option<Instance>,
}

// Tells rust that it is safe to send this c_void pointer
// across lambda boundaries
#[derive(Clone)]
struct BorrowBypass {
    fn_pointer: *mut c_void,
}

unsafe impl Send for BorrowBypass {}
unsafe impl Sync for BorrowBypass {}

impl WasmiRuntime {
    fn new(bytes: &[u8], userctx: *mut c_void) -> WasmiRuntime {
        let stack_limit = StackLimits::default();

        let engine = Engine::new(
            &Config::default()
                .consume_fuel(true)
                .floats(false)
                .set_stack_limits(stack_limit),
        );

        let store = Store::new(&engine, userctx);
        let module = Module::new(&engine, &bytes).unwrap();
        let linker = Linker::new(&engine);
        Self {
            module: module,
            store: store,
            linker: linker,
            instance: None,
        }
    }
    fn lazy_link(&mut self) -> Result<(), wasmi::Error> {
        match self.instance {
            Some(_) => {
                return Ok(());
            }
            None => {
                match self.linker.instantiate(&mut self.store, &self.module) {
                    Ok(inst_pre) => {
                        match inst_pre.ensure_no_start(&mut self.store) {
                            Ok(inst) => {
                                self.instance = Some(inst);
                                Ok(())
                            }
                            Err(x) => Err(x.into()),
                        }
                    }
                    Err(x) => Err(x),
                }
            }
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

        let func = Func::wrap(
            &mut self.store,
            move |caller: Caller<'_, *mut c_void>| -> Result<u64, wasmi::Error> {
                let res =
                    unsafe { external_call::c_call_0args(x.clone().fn_pointer, caller.data().clone()) };
                return external_call::handle_trampoline_error(res);
            },
        );

        match self.linker.define(import_name, fn_name, func) {
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

        let func = Func::wrap(
            &mut self.store,
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
        );

        match self.linker.define(import_name, fn_name, func) {
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

        let func = Func::wrap(
            &mut self.store,
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
        );

        match self.linker.define(import_name, fn_name, func) {
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

        let func = Func::wrap(
            &mut self.store,
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
        );

        match self.linker.define(import_name, fn_name, func) {
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

        let func = Func::wrap(
            &mut self.store,
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
        );

        match self.linker.define(import_name, fn_name, func) {
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

        let func = Func::wrap(
            &mut self.store,
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
        );

        match self.linker.define(import_name, fn_name, func) {
            Ok(_) => Ok(()),
            Err(err) => Err(err.into()),
        }
    }
}

fn assert_runtime_not_null(runtime: *const WasmiRuntime) {
    assert!(runtime != core::ptr::null());
}

#[repr(C)]
pub struct MemorySlice {
    mem: *mut u8,
    sz: u32,
}

#[no_mangle]
pub extern "C" fn wasmi_get_memory(runtime_void: *mut c_void) -> MemorySlice {

    let runtime : *mut WasmiRuntime = unsafe { core::mem::transmute (runtime_void) };
    assert_runtime_not_null(runtime);

    let r = unsafe { &mut *runtime };

    match r.lazy_link() {
        Ok(_) => (),
        Err(_) => {
            return MemorySlice {
                mem: std::ptr::null_mut(),
                sz: 0,
            };
        }
    };

    match r
        .instance
        .as_mut()
        .expect("lazily linked")
        .get_memory(&r.store, "memory")
    {
        Some(mem) => {
            let mem_sz = mem.data_size(&r.store) as u32;
            // wasmi docs say that _any_ call to data_ptr or use of store
            // can invalidate returned pointers
            MemorySlice {
                mem: mem.data_ptr(&r.store),
                sz: mem_sz,
            }
        }
        _ => MemorySlice {
            mem: std::ptr::null_mut(),
            sz: 0,
        },
    }
}

#[no_mangle]
pub extern "C" fn wasmi_consume_gas(
    runtime_void: *mut c_void,
    gas_consumed: u64,
) -> bool {

    let runtime : *mut WasmiRuntime = unsafe { core::mem::transmute(runtime_void) };
    assert_runtime_not_null(runtime);

    let r = unsafe { &mut *runtime };

    let cur_fuel: u64 = r.store.get_fuel().unwrap();
    if cur_fuel > gas_consumed {
        r.store.set_fuel(cur_fuel - gas_consumed).unwrap();
        return true;
    }
    r.store.set_fuel(0).unwrap();
    return false;
}

#[no_mangle]
pub extern "C" fn wasmi_get_available_gas(runtime_void: *const c_void) -> u64 {

    let runtime = unsafe { core::mem::transmute(runtime_void) };
    assert_runtime_not_null(runtime);
    let r = unsafe { &*runtime };
    return r.store.get_fuel().unwrap();
}

#[no_mangle]
pub extern "C" fn wasmi_set_available_gas(
    runtime_void: *mut c_void,
    gas: u64,
) {
    let runtime : *mut WasmiRuntime = unsafe { core::mem::transmute(runtime_void) };
    assert_runtime_not_null(runtime);
    let r = unsafe { &mut *runtime };
    r.store.set_fuel(gas).unwrap();
}

fn string_from_parts(bytes: *const u8, len: u32) -> Result<String, Utf8Error> {
    let slice = unsafe { slice::from_raw_parts(bytes, len as usize) };
    match std::str::from_utf8(&slice) {
        Ok(s) => Ok(s.to_owned()),
        Err(x) => Err(x),
    }
}

#[no_mangle]
pub extern "C" fn wasmi_link_nargs(
    runtime_void: *mut c_void,
    module_name: *const u8,
    module_name_len: u32,
    method_name: *const u8,
    method_name_len: u32,
    function_pointer: *mut c_void,
    nargs: u8,
) -> bool // true if success
{
    let runtime : *mut WasmiRuntime = unsafe { core::mem::transmute(runtime_void) };
    assert_runtime_not_null(runtime);

    assert!(module_name != core::ptr::null());
    assert!(method_name != core::ptr::null());

    // assert module name & method_name nonzero

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

    let r = unsafe { &mut *runtime };

    let res = match nargs {
        0 => r.link_function_0args(function_pointer, &module, &method),
        1 => r.link_function_1args(function_pointer, &module, &method),
        2 => r.link_function_2args(function_pointer, &module, &method),
        3 => r.link_function_3args(function_pointer, &module, &method),
        4 => r.link_function_4args(function_pointer, &module, &method),
        5 => r.link_function_5args(function_pointer, &module, &method),
        _ => {
            return false;
        }
    };

    match res {
        Ok(_) => true,
        Err(_) => false,
    }
}

fn handle_trap_code(code: wasmi::core::TrapCode) -> WasmiInvokeResult {
    // all of the wasmi trap codes, in version 0.36,
    // are raised deterministically.
    // For now, at least, we handle all as deterministic errors.
    match code {
        wasmi::core::TrapCode::OutOfFuel => {
            return WasmiInvokeResult::error(WasmiInvokeError::OutOfGasError);
        }
        _ => {
            return WasmiInvokeResult::error(
                WasmiInvokeError::DeterministicCallError,
            );
        }
    }
}

#[no_mangle]
pub extern "C" fn wasmi_invoke(
    runtime_void: *mut c_void,
    method_name: *const u8,
    method_name_len: u32,
) -> WasmiInvokeResult {
    let runtime : *mut WasmiRuntime = unsafe { core::mem::transmute(runtime_void) };

    assert_runtime_not_null(runtime);

    if method_name == core::ptr::null() {
        return WasmiInvokeResult::error(WasmiInvokeError::InputError);
    }

    let method_name_slice =
        unsafe { slice::from_raw_parts(method_name, method_name_len as usize) };

    let string = match std::str::from_utf8(&method_name_slice) {
        Ok(v) => v,
        _ => return WasmiInvokeResult::error(WasmiInvokeError::InputError),
    };

    let r = unsafe { &mut *runtime };

    match r.lazy_link() {
        Ok(_) => {}
        Err(_) => {
            return WasmiInvokeResult::error(WasmiInvokeError::LinkError);
        }
    };

    let func = match r
        .instance
        .as_mut()
        .expect("lazily linked")
        .get_func(&r.store, string)
    {
        Some(v) => v,
        _ => {
            return WasmiInvokeResult::error(WasmiInvokeError::FuncNExist);
        }
    };

    let mut res = [Val::I64(0)];

    let func_res = func.call(&mut r.store, &[], &mut res);

    match func_res {
        Ok(_) => match res[0].i64() {
            Some(v) => {
                return WasmiInvokeResult::success(v as u64);
            },
            _ => {
                return WasmiInvokeResult::error(
                    WasmiInvokeError::ReturnTypeError,
                );
            }
        },
        Err(err) => {
            match err.as_trap_code() {
                Some(trap_code) => {
                    return handle_trap_code(trap_code);
                }
                _ => {}
            };
            let my_error =
                match err.downcast_ref::<external_call::TrampolineError>() {
                    Some(trampoline_error) => trampoline_error.clone(),
                    None => {
                        return WasmiInvokeResult::error(
                            WasmiInvokeError::UnrecoverableSystemError,
                        );
                    }
                };

            if my_error.recoverable {
                return WasmiInvokeResult::error(
                    WasmiInvokeError::OutOfGasError,
                );
            }
            return WasmiInvokeResult::error(
                WasmiInvokeError::UnrecoverableSystemError,
            );
        }
    };
}

#[no_mangle]
pub extern "C" fn new_wasmi_runtime(
    bytes: *const u8,
    bytes_len: u32,
    userctx: *mut c_void,
) -> *mut c_void {
    if bytes == core::ptr::null() {
        return core::ptr::null_mut();
    }

    let slice = unsafe { slice::from_raw_parts(bytes, bytes_len as usize) };

    let b = Box::new(WasmiRuntime::new(&slice, userctx));

    return unsafe {core::mem::transmute(Box::into_raw(b)) };
}

#[no_mangle]
pub extern "C" fn free_wasmi_runtime(p: *mut c_void) {
    let runtime : *mut WasmiRuntime = unsafe { core::mem::transmute(p) };

    assert_runtime_not_null(runtime);

    unsafe { drop(Box::from_raw(runtime)) };
}
