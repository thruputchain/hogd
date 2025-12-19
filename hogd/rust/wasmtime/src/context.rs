use core::ffi::c_void;
use core::slice;

use common::string_from_parts;

use wasmtime::{
    Caller, Config, Engine, InstanceAllocationStrategy, Linker, Module,
    PoolingAllocationConfig,
};

use crate::error::TrampolineError;
use crate::module_cache::{ModuleCache, ModuleCacheEntry};
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

#[derive(Clone)]
pub struct WasmtimeGlobalContext {
    pub engine: Engine,
    // TODO(geoff): we could stick the Linker here
}

impl WasmtimeGlobalContext {
    fn new() -> WasmtimeGlobalContext {
        let page_size: usize = 65536; // 64KB, i.e. 1 page
        let allocation_config: PoolingAllocationConfig =
            PoolingAllocationConfig::default()
                .max_unused_warm_slots(8192)
                .total_core_instances(8192)
                .total_memories(8192)
                .total_tables(8192)
                .linear_memory_keep_resident(page_size * 2)
                .max_memory_size(page_size * 2)
                .table_keep_resident(20000)
                .clone();
        // .table_elements(max_size: default is 20k, leaving that as is)

        Self {
            engine: Engine::new(
                Config::default()
                    .consume_fuel(true)
                    .wasm_backtrace(false)
                    .strategy(wasmtime::Strategy::Winch)
                    .cranelift_opt_level(wasmtime::OptLevel::Speed)
                    .cranelift_nan_canonicalization(true) // deterministic nan
                    .relaxed_simd_deterministic(true)
                    .wasm_multi_memory(false)
                    .epoch_interruption(false)
                    .max_wasm_stack(16 * 1024)
                    // Configs set as per wasmtime documentation to eliminate vmm in steady state
                    // https://docs.rs/wasmtime/latest/wasmtime/struct.PoolingAllocationConfig.html
                    .memory_init_cow(false)
                    .memory_guard_size(0)
                    .memory_reservation(2 * page_size as u64)
                    .guard_before_linear_memory(false)
                    .allocation_strategy(InstanceAllocationStrategy::Pooling(
                        allocation_config,
                    )),
            )
            .unwrap(),
        }
    }
}

pub struct WasmtimeContext<'a> {
    pub global_context: WasmtimeGlobalContext,
    pub entries: Vec<LinkEntry>,
    pub module_cache: Option<&'a mut ModuleCache>,
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

impl<'a> WasmtimeContext<'a> {
    fn new(
        global_context: &WasmtimeGlobalContext,
        module_cache: Option<&'a mut ModuleCache>,
    ) -> WasmtimeContext<'a> {
        Self {
            global_context: global_context.clone(),
            entries: Vec::new(),
            module_cache,
        }
    }

    pub fn has_module_cache(&self) -> bool {
        self.module_cache.is_some()
    }

    pub fn get_cached_module(
        &mut self,
        hash: &[u8; 32],
    ) -> Option<ModuleCacheEntry> {
        self.module_cache.as_mut().and_then(|cache| cache.get(hash))
    }

    pub fn put_cached_module(
        &mut self,
        hash: &[u8; 32],
        module: &Module,
        linker: &Linker<*mut c_void>,
    ) {
        if let Some(cache) = self.module_cache.as_mut() {
            cache.put(hash, module, linker);
        }
    }

    pub fn add_entry(&mut self, entry: LinkEntry) {
        self.entries.push(entry);
    }

    pub fn linker(&self) -> Linker<*mut c_void> {
        let engine = &self.global_context.engine;
        let mut linker = Linker::new(engine);

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
        let engine = &self.global_context.engine;
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
/// Context must have been created by new_wasmtime_context.
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

/// # Safety
/// Creates a global context that can be shared across threads.
/// Global context is threadsafe.
#[no_mangle]
pub unsafe extern "C" fn new_wasmtime_global_context() -> *mut c_void {
    let b = Box::new(WasmtimeGlobalContext::new());
    unsafe { core::mem::transmute(Box::into_raw(b)) }
}

/// # Safety
/// Global context must not be a nullptr.
/// Global context must have been created by new_wasmtime_global_context.
#[no_mangle]
pub unsafe extern "C" fn free_wasmtime_global_context(p: *mut c_void) {
    debug_assert!(!p.is_null());

    let context: *mut WasmtimeGlobalContext =
        unsafe { core::mem::transmute(p) };

    unsafe { drop(Box::from_raw(context)) };
}

// Rust FFI needs no_mangle and extern "C"
/// # Safety
/// global_context must not be nullptr and must have been created by new_wasmtime_global_context.
/// module_cache can be nullptr, in which case no module cache is used.
/// module_cache must outlive WasmtimeContext.
#[no_mangle]
pub unsafe extern "C" fn new_wasmtime_context(
    global_context: *mut c_void,
    module_cache: *mut c_void,
) -> *mut c_void {
    debug_assert!(!global_context.is_null());
    let global_context: &WasmtimeGlobalContext = unsafe {
        &*core::mem::transmute::<*mut c_void, *mut WasmtimeGlobalContext>(
            global_context,
        )
    };

    let b = if module_cache.is_null() {
        Box::new(WasmtimeContext::new(global_context, None))
    } else {
        Box::new(WasmtimeContext::new(global_context, unsafe {
            Some(
                &mut *(core::mem::transmute::<*mut c_void, *mut ModuleCache>(
                    module_cache,
                )),
            )
        }))
    };

    unsafe { core::mem::transmute(Box::into_raw(b)) }
}

/// # Safety
/// Caller must have exclusive (non-concurrent) access to the context, and context must not be a nullptr.
/// Context must have been created by new_wasmtime_context.
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
