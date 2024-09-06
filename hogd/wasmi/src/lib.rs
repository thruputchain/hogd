use wasmi::{Caller, Config, Engine, Error, Func, Instance, Linker, Module, Store, Val, StackLimits};

use core::ffi::c_void;
use std::slice;

use core::str::Utf8Error;

mod external_call;

pub struct WasmiRuntime {
    store: Store<*mut c_void>,
    module: Module,
    linker: Linker<*mut c_void>,
    instance: Option<Instance>,
}

#[derive(Clone)]
pub struct BorrowBypass {
    fn_pointer: *mut c_void,
}

unsafe impl Send for BorrowBypass {}
unsafe impl Sync for BorrowBypass {}

impl WasmiRuntime {
    pub fn new(
        bytes: &[u8],
        userctx: *mut c_void,
    ) -> WasmiRuntime {

        let stack_limit = StackLimits::default();

        let engine = Engine::new(&Config::default().consume_fuel(true).floats(false).set_stack_limits(stack_limit));

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
    pub fn lazy_link(&mut self) -> Result<(), wasmi::Error> {
        match self.instance {
            Some(_) => {
                return Ok(());
            }
            None => match self.linker.instantiate(&mut self.store, &self.module) {
                Ok(inst_pre) => match inst_pre.ensure_no_start(&mut self.store) {
                    Ok(inst) => {
                        self.instance = Some(inst);
                        Ok(())
                    }
                    Err(x) => Err(x.into()),
                },
                Err(x) => Err(x),
            },
        }
    }

    pub fn link_function_0args(
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
                // This is necessary to stop some part of rust
                // from complaining
                let _y = x.clone();
                let res = unsafe { external_call::c_call_0args(x.fn_pointer, caller.data().clone()) };
                return external_call::handle_trampoline_error(res);
            },
        );

        match self.linker.define(import_name, fn_name, func) {
            Ok(_) => Ok(()),
            Err(err) => Err(err.into()),
        }
    }

    pub fn link_function_1args(
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
            move |caller: Caller<'_, *mut c_void>, arg1: u64| -> Result<u64, wasmi::Error> {
                // This is necessary to stop some part of rust
                // from complaining
                let _y = x.clone();
                let res = unsafe { external_call::c_call_1args(x.fn_pointer, caller.data().clone(), arg1) };
                return external_call::handle_trampoline_error(res);
            },
        );

        match self.linker.define(import_name, fn_name, func) {
            Ok(_) => Ok(()),
            Err(err) => Err(err.into()),
        }
    }

    pub fn link_function_2args(
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
                // This is necessary to stop some part of rust
                // from complaining
                let _y = x.clone();
                let res = unsafe { external_call::c_call_2args(x.fn_pointer, caller.data().clone(), arg1, arg2) };
                return external_call::handle_trampoline_error(res);
            },
        );

        match self.linker.define(import_name, fn_name, func) {
            Ok(_) => Ok(()),
            Err(err) => Err(err.into()),
        }
    }

    pub fn link_function_3args(
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
                // This is necessary to stop some part of rust
                // from complaining
                let _y = x.clone();
                let res =
                    unsafe { external_call::c_call_3args(x.fn_pointer, caller.data().clone(), arg1, arg2, arg3) };
                return external_call::handle_trampoline_error(res);
            },
        );

        match self.linker.define(import_name, fn_name, func) {
            Ok(_) => Ok(()),
            Err(err) => Err(err.into()),
        }
    }

    pub fn link_function_4args(
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
                // This is necessary to stop some part of rust
                // from complaining
                let _y = x.clone();
                let res = unsafe {
                    external_call::c_call_4args(x.fn_pointer, caller.data().clone(), arg1, arg2, arg3, arg4)
                };
                return external_call::handle_trampoline_error(res);
            },
        );

        match self.linker.define(import_name, fn_name, func) {
            Ok(_) => Ok(()),
            Err(err) => Err(err.into()),
        }
    }

    pub fn link_function_5args(
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
                // This is necessary to stop some part of rust
                // from complaining
                let _y = x.clone();
                let res = unsafe {
                    external_call::c_call_5args(
                        x.fn_pointer,
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

enum WasmiInvokeError {
    None = 0,
    LinkError = 1,
    FuncNExist = 2,
    InputError = 3, // input validation fails
    ReturnTypeError = 4,
    CallError = 5,
    HostError = 6,
    UnrecoverableSystemError = 7,
}

#[repr(C)]
pub struct WasmiInvokeResult {
    result: u64,
    error: u32,
}

#[repr(C)]
pub struct MemorySlice {
    mem: *mut u8,
    sz: u32,
}

#[no_mangle]
pub extern "C" fn wasmi_get_memory(runtime: *mut WasmiRuntime) -> MemorySlice {
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
pub extern "C" fn wasmi_consume_gas(runtime: *mut WasmiRuntime, gas_consumed: u64) -> bool {
    let r = unsafe { &mut *runtime };

    let cur_fuel: u64 = r.store.get_fuel().unwrap();
    if cur_fuel > gas_consumed {
        r.store.set_fuel(cur_fuel - gas_consumed).unwrap();
        return false;
    }
    r.store.set_fuel(0).unwrap();
    return true;
}

#[no_mangle]
pub extern "C" fn wasmi_get_available_gas(runtime: *const WasmiRuntime) -> u64 {
    let r = unsafe { &*runtime };
    return r.store.get_fuel().unwrap();
}

#[no_mangle]
pub extern "C" fn wasmi_set_available_gas(runtime: *mut WasmiRuntime, gas : u64) {
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
    runtime: *mut WasmiRuntime,
    module_name: *const u8,
    module_name_len: u32,
    method_name: *const u8,
    method_name_len: u32,
    function_pointer: *mut c_void,
    nargs: u8,
) -> bool // true if success
{
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

#[no_mangle]
pub extern "C" fn wasmi_invoke(
    runtime: *mut WasmiRuntime,
    bytes: *const u8,
    bytes_len: u32,
) -> WasmiInvokeResult {
    let slice = unsafe { slice::from_raw_parts(bytes, bytes_len as usize) };

    let string = match std::str::from_utf8(&slice) {
        Ok(v) => v,
        _ => {
            return WasmiInvokeResult {
                result: 0,
                error: WasmiInvokeError::InputError as u32,
            }
        }
    };

    let r = unsafe { &mut *runtime };

    match r.lazy_link() {
        Ok(_) => {}
        Err(_) => {
            return WasmiInvokeResult {
                result: 0,
                error: WasmiInvokeError::LinkError as u32,
            };
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
            return WasmiInvokeResult {
                result: 0,
                error: WasmiInvokeError::FuncNExist as u32,
            };
        }
    };

    let mut res = [Val::I64(0)];

    let func_res = func.call(&mut r.store, &[], &mut res);

    match func_res {
        Ok(_) => match res[0].i64() {
            Some(v) => {
                return WasmiInvokeResult {
                    result: v as u64,
                    error: WasmiInvokeError::None as u32,
                };
            }
            _ => {
                return WasmiInvokeResult {
                    result: 0,
                    error: WasmiInvokeError::ReturnTypeError as u32,
                };
            }
        },
        Err(err) => {
            let errstr = err.to_string();
            //janky but it works
            if errstr == "HostError" {
                return WasmiInvokeResult {
                    result: 0,
                    error: WasmiInvokeError::HostError as u32,
                };
            }
            if errstr == "UnrecoverableSystemError" {
                return WasmiInvokeResult {
                    result: 0,
                    error: WasmiInvokeError::UnrecoverableSystemError as u32,
                };
            }
            return WasmiInvokeResult {
                result: 0,
                error: WasmiInvokeError::CallError as u32,
            };
        }
    };
}

#[no_mangle]
pub fn new_wasmi_runtime(
    bytes: *const u8,
    bytes_len: u32,
    userctx: *mut c_void,
) -> *mut WasmiRuntime {
    let slice = unsafe { slice::from_raw_parts(bytes, bytes_len as usize) };

    let b = Box::new(WasmiRuntime::new(
        &slice,
        userctx,
    ));

    return Box::into_raw(b);
}

#[no_mangle]
pub extern "C" fn free_wasmi_runtime(p: *mut WasmiRuntime) {
    unsafe { drop(Box::from_raw(p)) };
}
