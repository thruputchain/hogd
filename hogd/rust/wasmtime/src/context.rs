use core::ffi::c_void;
use core::slice;
use std::sync::Arc;

use crate::debug_log;
use common::string_from_parts;

use wasmtime::{
    Caller, Config, Engine, InstanceAllocationStrategy, Linker, Module,
    PoolingAllocationConfig, InstancePre,
};

use crate::custom_code_memory::MprotectCodeMemory;
use crate::error::TrampolineError;
use crate::SHA256Hash;
pub use common::contract_loader::ContractGetFn;
pub use common::contract_loader::ContractGetResult;
use common::SystemErrorCode;
use common::TrampolineResult;
use crate::instance_pre_cache::InstancePreGetFn;
use crate::instance_pre_cache::InstancePrePutFn;
use crate::instance_pre_cache::InstancePreCache;

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
// assert that the memory is exported and starts at at most 1 page
fn check_memory_limit(module: &Module) -> bool {
    let export = module.get_export("memory");
    if export.is_none() {
        debug_log!(eprintln!("\"memory\" not exported from module"));
        return false;
    }
    let export_unwrapped = export.unwrap();

    if let wasmtime::ExternType::Memory(memory_type) = export_unwrapped {
        debug_log!(if memory_type.minimum() > 1u64 {
            eprintln!("Wasm initial memory exceeds limit: {memory_type:?}")
        });
        return memory_type.minimum() <= 1u64;
    }
    debug_log!(eprintln!("\"memory\" not exported as wasm memory"));
    false
}

fn check_no_start(module: &Module) -> bool {
    let export = module.get_export("start");
    export.is_none()
}

#[derive(Clone)]
pub struct WasmtimeGlobalContext {
    engine: Engine,
    instance_pre_cache: InstancePreCache,
    // TODO(geoff): we could stick the Linker here
}

impl WasmtimeGlobalContext {
    fn new_engine() -> Engine {
        let page_size: usize = 65536; // 64KB, i.e. 1 page
        let num_slots: u32 = 1024;
        let table_size: usize = 64;
        let allocation_config: PoolingAllocationConfig =
            PoolingAllocationConfig::default()
                .max_unused_warm_slots(num_slots)
                .total_core_instances(num_slots)
                .total_memories(num_slots)
                .total_tables(num_slots)
                .linear_memory_keep_resident(page_size * 2)
                .max_memory_size(page_size * 2)
                .table_keep_resident(table_size)
                .table_elements(table_size)
                //.memory_protection_keys(wasmtime::Enabled::No)
                .clone();

        let mut config = Config::default();
        config
            .consume_fuel(true)
            .wasm_backtrace(false)
            .strategy(wasmtime::Strategy::Winch)
            // cranelift-only options
            //.signals_based_traps(false)
            //.table_lazy_init(true)
            // done cranelift-only
            .cranelift_pcc(false)
            .native_unwind_info(false)
            .wasm_backtrace_details(wasmtime::WasmBacktraceDetails::Disable)
            .wasm_backtrace(false)
            .debug_info(false)
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
            .memory_reservation_for_growth(0)
            .guard_before_linear_memory(false)
            .memory_may_move(false)
            .generate_address_map(false)
            .allocation_strategy(InstanceAllocationStrategy::Pooling(
                allocation_config,
            ));

        //config.with_custom_code_memory(Some(Arc::new(MprotectCodeMemory::new())));

        Engine::new(&config).unwrap()
    }

    pub fn new(
        instance_pre_cache: InstancePreCache,
    ) -> WasmtimeGlobalContext {
        Self {
            engine: Self::new_engine(),
            instance_pre_cache,
        }
    }
}

pub struct WasmtimeContext {
    global_context: WasmtimeGlobalContext,
    linker: Linker<*mut c_void>,
    contract_get_fn: ContractGetFn,
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
    fn new(
        global_context: &WasmtimeGlobalContext,
        get_fn: ContractGetFn,
    ) -> WasmtimeContext {
        Self {
            global_context: global_context.clone(),
            linker: Linker::new(&global_context.engine),
            contract_get_fn: get_fn,
        }
    }

    /// # Safety
    /// user_context must be a valid pointer to a HostCallContext (or a nullptr).
    pub unsafe fn fetch_contract<'a>(
        &self,
        user_context: *mut c_void,
        hash: &'a SHA256Hash,
    ) -> Result<Option<&'a [u8]>, SystemErrorCode> {
        let get_res =
            unsafe { (self.contract_get_fn)(user_context, hash.as_ptr()) };
        get_res.to_data()
    }

    pub fn engine(&self) -> &Engine {
        &self.global_context.engine
    }

    pub fn get_instance_pre(&mut self, hash: &[u8; 32], user_context: *mut c_void) -> Result<Option<InstancePre<*mut c_void>>, SystemErrorCode> {
        
        if let Some(instance_pre) = self.global_context.instance_pre_cache.get(hash) {
            return Ok(Some(instance_pre));
        }
        // otherwise compile the module and cache it
        let bytes = unsafe { self.fetch_contract(user_context, hash)? };
        if bytes.is_none() {
            return Ok(None);
        }
        let bytes = bytes.unwrap();
        let linker = self.linker();
        let module = Module::new(linker.engine(), bytes)
            .inspect_err(|_e| {
                debug_log!(eprintln!("Module new error: {_e}"))
            })
            .map_err(|_e| SystemErrorCode::kSystemUnrecoverable)?;

        if !check_memory_limit(&module) {
            debug_log!(eprintln!("Module instantiation failed: Memory export verification failed"));
            return Ok(None);
        }
        if !check_no_start(&module) {
            debug_log!(eprintln!("Module instantiation failed: Has start function"));
            return Ok(None);
        }

        let instance_pre = self.linker.instantiate_pre(&module).inspect_err(|_e| {
            debug_log!(eprintln!("Error instantiating store: {_e}"));
        }).map_err(|_e| SystemErrorCode::kSystemUnrecoverable)?;

        self.global_context.instance_pre_cache.put(hash, &instance_pre);

        return Ok(Some(instance_pre));
    }

    pub fn link_function(&mut self, entry: LinkEntry) {
        match entry.nargs {
            0 => Self::link_function_0args(
                &mut self.linker,
                entry.fn_pointer,
                &entry.module,
                &entry.function,
            ),
            1 => Self::link_function_1args(
                &mut self.linker,
                entry.fn_pointer,
                &entry.module,
                &entry.function,
            ),
            2 => Self::link_function_2args(
                &mut self.linker,
                entry.fn_pointer,
                &entry.module,
                &entry.function,
            ),
            3 => Self::link_function_3args(
                &mut self.linker,
                entry.fn_pointer,
                &entry.module,
                &entry.function,
            ),
            4 => Self::link_function_4args(
                &mut self.linker,
                entry.fn_pointer,
                &entry.module,
                &entry.function,
            ),
            5 => Self::link_function_5args(
                &mut self.linker,
                entry.fn_pointer,
                &entry.module,
                &entry.function,
            ),
            6 => Self::link_function_6args(
                &mut self.linker,
                entry.fn_pointer,
                &entry.module,
                &entry.function,
            ),
            7 => Self::link_function_7args(
                &mut self.linker,
                entry.fn_pointer,
                &entry.module,
                &entry.function,
            ),
            8 => Self::link_function_8args(
                &mut self.linker,
                entry.fn_pointer,
                &entry.module,
                &entry.function,
            ),
            _ => panic!("Unsupported number of arguments: {}", entry.nargs),
        }
        .unwrap();
    }

    pub fn linker(&self) -> &Linker<*mut c_void> {
        &self.linker
    }

    pub fn verify_contract(&self, contract_data: &[u8]) -> bool {
        let engine = &self.global_context.engine;
        Module::validate(&engine, contract_data).is_ok()
        // TODO(geoff): consider validating that contract memory exports are good and no start function
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

    c.link_function(LinkEntry::new(module, method, nargs, function_pointer));
    true
}

/// # Safety
/// Creates a global context that can be shared across threads.
/// Global context is threadsafe.
#[no_mangle]
pub unsafe extern "C" fn new_wasmtime_global_context(
    instance_pre_get_fn: InstancePreGetFn,
    instance_pre_put_fn: InstancePrePutFn,
    external_cache: *mut c_void,
) -> *mut c_void {
    let instance_pre_cache = InstancePreCache::new(
        instance_pre_get_fn,
        instance_pre_put_fn,
        external_cache,
    );
    let b = Box::new(WasmtimeGlobalContext::new(instance_pre_cache));
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
    get_fn: ContractGetFn,
    global_context: *mut c_void,
) -> *mut c_void {

    debug_assert!(!global_context.is_null());

    let global_context: &WasmtimeGlobalContext = unsafe {
        &*core::mem::transmute::<*mut c_void, *mut WasmtimeGlobalContext>(
            global_context,
        )
    };

    let b = Box::new(WasmtimeContext::new(
        global_context,
        get_fn,
    ));
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
