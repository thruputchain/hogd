use core::ffi::c_void;
use std::cell::Cell;
use std::sync::atomic::{AtomicUsize, Ordering};
use std::sync::Arc;

use crate::debug_log;
use common::string_from_parts;

use wasmtime::{
    Caller, Config, Engine, InstanceAllocationStrategy, Linker,
    PoolingAllocationConfig,
};

use crate::error::TrampolineError;
use crate::instance_pre_cache::InstancePreCache;
use common::TrampolineResult;

#[derive(Clone)]
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

pub struct WasmtimeGlobalContext {
    engines: Vec<Engine>,
    // We get slight contention (on the payments-10 benchmark, apparently around 1-2%, around 465/470K->460/455K or so)
    // from contention on one InstancePreCache object, as compared to having one per engine.
    // The savings is that we significantly reduce memory usage (as the number of contracts cached increases).
    // I (geoff) made the choice to do one shared cache because
    // (1) we already have some shared caches in hogexec (the snapshot cache), and while contention there is not insignificant,
    // the performance impact is tolerable.
    // (2) The performance change of having one shared cache is not extremely large on maximal contention workloads (with very few cache entries),
    // and I am more concerned with how performance will degrade as the number of cache entries increases
    // (and memory usage increases).
    //
    // But this change can be revisted if necessary.
    instance_pre_cache: InstancePreCache,
    engine_count: usize,
    linkers: Vec<Linker<*mut c_void>>,
}

impl WasmtimeGlobalContext {
    /// Allocates an engine index using a static thread-local atomic pattern,
    /// similar to the C++ implementation. Each thread gets a stable engine index
    /// that is assigned on first access.
    pub fn alloc_engine_index(&self) -> usize {
        static ALLOCATOR: AtomicUsize = AtomicUsize::new(0);
        thread_local! {
            static THREAD_LOCAL_ALLOC: Cell<Option<usize>> = const { Cell::new(None) };
        }

        THREAD_LOCAL_ALLOC.with(|cell| {
            if let Some(idx) = cell.get() {
                idx
            } else {
                let idx = ALLOCATOR.fetch_add(1, Ordering::Relaxed)
                    % self.engine_count;
                cell.set(Some(idx));
                idx
            }
        })
    }

    fn new_engine() -> Engine {
        let page_size: usize = 65536; // 64KB, i.e. 1 page
        let num_slots: u32 = 1024;
        let table_size: usize = 64;
        let allocation_config: PoolingAllocationConfig =
            PoolingAllocationConfig::default()
                .max_unused_warm_slots(0)
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
        engine_count: usize,
        instance_pre_cache: InstancePreCache,
    ) -> WasmtimeGlobalContext {
        // Create vector of engines
        let engines: Vec<Engine> =
            (0..engine_count).map(|_| Self::new_engine()).collect();
        // Create one linker per engine
        let linkers: Vec<Linker<*mut c_void>> =
            engines.iter().map(Linker::new).collect();
        Self {
            engines,
            instance_pre_cache,
            engine_count,
            linkers,
        }
    }

    pub fn engine(&self, engine_index: usize) -> &Engine {
        debug_assert!(engine_index < self.engine_count);
        &self.engines[engine_index]
    }

    pub fn engine_count(&self) -> usize {
        self.engine_count
    }

    pub fn linker(&self, engine_index: usize) -> &Linker<*mut c_void> {
        debug_assert!(engine_index < self.engine_count);
        &self.linkers[engine_index]
    }

    pub fn instance_pre_cache(&self) -> &InstancePreCache {
        &self.instance_pre_cache
    }

    pub fn link_function(&mut self, entry: LinkEntry) {
        // Apply to all existing linkers
        for linker in &mut self.linkers {
            Self::apply_link_entry(linker, &entry);
        }
    }

    fn apply_link_entry(linker: &mut Linker<*mut c_void>, entry: &LinkEntry) {
        match entry.nargs {
            0 => Self::link_function_0args(
                linker,
                entry.fn_pointer,
                &entry.module,
                &entry.function,
            ),
            1 => Self::link_function_1args(
                linker,
                entry.fn_pointer,
                &entry.module,
                &entry.function,
            ),
            2 => Self::link_function_2args(
                linker,
                entry.fn_pointer,
                &entry.module,
                &entry.function,
            ),
            3 => Self::link_function_3args(
                linker,
                entry.fn_pointer,
                &entry.module,
                &entry.function,
            ),
            4 => Self::link_function_4args(
                linker,
                entry.fn_pointer,
                &entry.module,
                &entry.function,
            ),
            5 => Self::link_function_5args(
                linker,
                entry.fn_pointer,
                &entry.module,
                &entry.function,
            ),
            6 => Self::link_function_6args(
                linker,
                entry.fn_pointer,
                &entry.module,
                &entry.function,
            ),
            7 => Self::link_function_7args(
                linker,
                entry.fn_pointer,
                &entry.module,
                &entry.function,
            ),
            8 => Self::link_function_8args(
                linker,
                entry.fn_pointer,
                &entry.module,
                &entry.function,
            ),
            _ => panic!("Unsupported number of arguments: {}", entry.nargs),
        }
        .unwrap();
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
/// Global context must not be a nullptr.
/// Global context must have been created by new_wasmtime_global_context.
#[no_mangle]
pub unsafe extern "C" fn wasmtime_global_link_nargs(
    global_context_void: *mut c_void,
    module_name: *const u8,
    module_name_len: u32,
    method_name: *const u8,
    method_name_len: u32,
    function_pointer: *mut c_void,
    nargs: u8,
) -> bool // true if success
{
    let global_context: &mut Arc<WasmtimeGlobalContext> = unsafe {
        &mut *core::mem::transmute::<*mut c_void, *mut Arc<WasmtimeGlobalContext>>(
            global_context_void,
        )
    };

    debug_assert!(!global_context_void.is_null());
    debug_assert!(!module_name.is_null());
    debug_assert!(!method_name.is_null());

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

    // Get mutable reference from Arc - this only works if there's exactly one reference
    // Linking should happen before contexts are created
    if let Some(global_context_mut) = Arc::get_mut(global_context) {
        global_context_mut.link_function(LinkEntry::new(
            module,
            method,
            nargs,
            function_pointer,
        ));
        true
    } else {
        debug_log!(eprintln!("Multiple references exist to global context"));
        // Multiple references exist - this shouldn't happen if linking is done before contexts
        false
    }
}

/// # Safety
/// Creates a global context that can be shared across threads.
/// Global context is threadsafe.
/// engine_count must be greater than 0.
#[no_mangle]
pub unsafe extern "C" fn new_wasmtime_global_context(
    engine_count: usize,
) -> *mut c_void {
    debug_assert!(engine_count > 0);
    let instance_pre_cache = InstancePreCache::new(engine_count);
    let global_context =
        Arc::new(WasmtimeGlobalContext::new(engine_count, instance_pre_cache));
    unsafe { core::mem::transmute(Box::into_raw(Box::new(global_context))) }
}

/// # Safety
/// Global context must not be a nullptr.
/// Global context must have been created by new_wasmtime_global_context.
#[no_mangle]
pub unsafe extern "C" fn free_wasmtime_global_context(p: *mut c_void) {
    debug_assert!(!p.is_null());

    let context: *mut Arc<WasmtimeGlobalContext> =
        unsafe { core::mem::transmute(p) };

    unsafe { drop(Box::from_raw(context)) };
}
