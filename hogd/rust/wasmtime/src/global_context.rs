use core::ffi::c_void;
use std::collections::HashMap;
use std::sync::atomic::{AtomicUsize, Ordering};
use std::sync::Arc;

use crate::debug_log;
use common::string_from_parts;
use common::LinkEntry;

use wasmtime::{
    Caller, Config, Engine, InstanceAllocationStrategy, Linker,
    PoolingAllocationConfig, WasmFeatures,
};

use crate::error::TrampolineError;
use crate::instance_pre_cache::InstancePreCache;

macro_rules! link_function_nargs {
    ($name:ident, $cfunc:ident $(, $arg:ident)*) => {
        fn $name(
            linker: &mut Linker<*mut c_void>,
            fn_pointer: common::ErasedFnPtr,
            import_name: &str,
            fn_name: &str,
        ) -> Result<(), wasmtime::Error> {
            let ptr = fn_pointer;
            linker
                .func_wrap(import_name, fn_name, move |caller: Caller<'_, *mut c_void>, $($arg: u64),*| {
                    let res : Result<i32, common::HostFnErrorCode> = {
                        common::$cfunc(
                            ptr,
                            *caller.data(),
                            $($arg),*
                        )
                    }.into();
                    res.map_err(|error| wasmtime::Error::new(TrampolineError { error }))
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
    /// Per-instance engine index allocator.
    /// Each call to alloc_engine_index atomically increments this and wraps
    /// modulo engine_count. This replaces the old static global allocator
    /// which was shared across all WasmtimeGlobalContext instances.
    next_engine_index: AtomicUsize,
    /// Shadow of host imports registered into every per-engine linker.
    /// Keyed by (module_name, field_name), value is the arity used to
    /// pick the `link_function_Nargs` wrapper. All host imports share
    /// the shape `[i64; nargs] -> [i32]`, so arity alone is sufficient
    /// to structurally validate a contract's declared import type.
    imports: HashMap<(String, String), u8>,
}

impl WasmtimeGlobalContext {
    /// Allocates an engine index from this instance's atomic counter.
    /// Round-robins across engines. Threadsafe — multiple threads can call
    /// this concurrently.
    pub fn alloc_engine_index(&self) -> usize {
        self.next_engine_index.fetch_add(1, Ordering::Relaxed)
            % self.engine_count
    }

    /// Build the production Wasmtime Config used by the hogd runtime.
    ///
    /// Public so tests can pin a target triple / CPU features on top of
    /// the same base config, keeping test-side engines in lockstep with
    /// production without hand-copying every knob. Production code
    /// constructs engines via `new_engine()`.
    pub fn new_config() -> Config {
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

        let compiler = wasmtime::Strategy::Winch;

        let mut config = Config::default();
        config
            .consume_fuel(true)
            .strategy(compiler)
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
            .cranelift_nan_canonicalization(true) // deterministic nan on cranelift
            // Note: nan canonicalization is not currently available on winch,
            // so we disable floats entirely. wasmtime's Config has no
            // high-level helper for this; the supported path is the
            // low-level wasm_features escape hatch, which clears the
            // FLOATS bit in wasmparser::WasmFeatures so f32/f64 ops and
            // types are rejected at validation.
            .wasm_features(WasmFeatures::FLOATS, false)
            // Disable SIMD. Winch's x86_64 SIMD lowering is gated on
            // runtime-detected AVX, and its aarch64 backend leaves SIMD
            // unimplemented. Leaving SIMD enabled would make contract
            // validity and runtime-creation success host-CPU dependent,
            // producing consensus divergence (audit #1469 / H002). With
            // wasm_simd(false), wasmparser rejects v128 operators at
            // validation uniformly across every host. wasm_relaxed_simd
            // is disabled for defense in depth — relaxed-SIMD requires
            // SIMD, so this is redundant but explicit.
            .wasm_simd(false)
            .wasm_relaxed_simd(false)
            .relaxed_simd_deterministic(true)
            .wasm_multi_memory(false)
            .epoch_interruption(false)
            // Wasmtime 42.0.0 sync stack notes:
            // - max_wasm_stack is a per-Store wasm limit, not a separate native
            //   stack allocation (wasmtime-42.0.0/src/runtime/func.rs:1463-1527).
            // - VMStoreContext::stack_limit defaults to usize::MAX when no wasm
            //   activation is live (wasmtime-42.0.0/src/runtime/vm/vmcontext.rs:1258-1264).
            // - same-Store wasm -> host -> wasm recursion reuses the existing
            //   limit, but fresh Stores recompute from the current SP
            //   (wasmtime-42.0.0/src/runtime/func.rs:1469-1482,
            //    wasmtime-42.0.0/src/runtime/func.rs:1518-1527).
            // This means host-only stack growth is still governed by the native
            // thread stack; this knob matters for wasm frames and same-Store
            // reentry.
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

        if compiler == wasmtime::Strategy::Cranelift {
            config.signals_based_traps(false);
            config.table_lazy_init(true);
        }

        if cfg!(feature = "wasm_debug") {
            config.wasm_backtrace(true);
            config
                .wasm_backtrace_details(wasmtime::WasmBacktraceDetails::Enable);

            #[cfg(feature = "wasm_debug")]
            if compiler == wasmtime::Strategy::Cranelift {
                // These options are not yet compatible with winch
                config.guest_debug(true);
                config.debug_info(true);
            }
        }

        //config.with_custom_code_memory(Some(Arc::new(MprotectCodeMemory::new())));

        config
    }

    fn new_engine() -> Engine {
        Engine::new(&Self::new_config()).unwrap()
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
            next_engine_index: AtomicUsize::new(0),
            imports: HashMap::new(),
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

    pub fn imports(&self) -> &HashMap<(String, String), u8> {
        &self.imports
    }

    pub fn link_function(&mut self, entry: LinkEntry) {
        // Apply to all existing linkers
        for linker in &mut self.linkers {
            Self::apply_link_entry(linker, &entry);
        }
        self.imports.insert(
            (entry.module().to_string(), entry.function().to_string()),
            entry.nargs(),
        );
    }

    fn apply_link_entry(linker: &mut Linker<*mut c_void>, entry: &LinkEntry) {
        match entry.nargs() {
            0 => Self::link_function_0args(
                linker,
                entry.fn_pointer(),
                entry.module(),
                entry.function(),
            ),
            1 => Self::link_function_1args(
                linker,
                entry.fn_pointer(),
                entry.module(),
                entry.function(),
            ),
            2 => Self::link_function_2args(
                linker,
                entry.fn_pointer(),
                entry.module(),
                entry.function(),
            ),
            3 => Self::link_function_3args(
                linker,
                entry.fn_pointer(),
                entry.module(),
                entry.function(),
            ),
            4 => Self::link_function_4args(
                linker,
                entry.fn_pointer(),
                entry.module(),
                entry.function(),
            ),
            5 => Self::link_function_5args(
                linker,
                entry.fn_pointer(),
                entry.module(),
                entry.function(),
            ),
            6 => Self::link_function_6args(
                linker,
                entry.fn_pointer(),
                entry.module(),
                entry.function(),
            ),
            7 => Self::link_function_7args(
                linker,
                entry.fn_pointer(),
                entry.module(),
                entry.function(),
            ),
            8 => Self::link_function_8args(
                linker,
                entry.fn_pointer(),
                entry.module(),
                entry.function(),
            ),
            9 => Self::link_function_9args(
                linker,
                entry.fn_pointer(),
                entry.module(),
                entry.function(),
            ),
            10 => Self::link_function_10args(
                linker,
                entry.fn_pointer(),
                entry.module(),
                entry.function(),
            ),
            11 => Self::link_function_11args(
                linker,
                entry.fn_pointer(),
                entry.module(),
                entry.function(),
            ),
            12 => Self::link_function_12args(
                linker,
                entry.fn_pointer(),
                entry.module(),
                entry.function(),
            ),
            _ => panic!("Unsupported number of arguments: {}", entry.nargs()),
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
    link_function_nargs!(
        link_function_9args,
        c_call_9args,
        arg1,
        arg2,
        arg3,
        arg4,
        arg5,
        arg6,
        arg7,
        arg8,
        arg9
    );
    link_function_nargs!(
        link_function_10args,
        c_call_10args,
        arg1,
        arg2,
        arg3,
        arg4,
        arg5,
        arg6,
        arg7,
        arg8,
        arg9,
        arg10
    );

    link_function_nargs!(
        link_function_11args,
        c_call_11args,
        arg1,
        arg2,
        arg3,
        arg4,
        arg5,
        arg6,
        arg7,
        arg8,
        arg9,
        arg10,
        arg11
    );
    link_function_nargs!(
        link_function_12args,
        c_call_12args,
        arg1,
        arg2,
        arg3,
        arg4,
        arg5,
        arg6,
        arg7,
        arg8,
        arg9,
        arg10,
        arg11,
        arg12
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
    function_pointer: common::ErasedFnPtr,
    nargs: u8,
) -> bool // true if success
{
    debug_assert!(!global_context_void.is_null());
    debug_assert!(!module_name.is_null());
    debug_assert!(!method_name.is_null());

    let global_context: &mut Arc<WasmtimeGlobalContext> = unsafe {
        &mut *global_context_void.cast::<Arc<WasmtimeGlobalContext>>()
    };

    let module =
        match unsafe { string_from_parts(module_name, module_name_len) } {
            Ok(x) => x,
            _ => {
                return false;
            }
        };

    let method =
        match unsafe { string_from_parts(method_name, method_name_len) } {
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
    assert!(engine_count > 0, "engine_count must be > 0");
    let instance_pre_cache = InstancePreCache::new(engine_count);
    let global_context =
        Arc::new(WasmtimeGlobalContext::new(engine_count, instance_pre_cache));
    Box::into_raw(Box::new(global_context)).cast::<c_void>()
}

/// # Safety
/// If p is null, this function is a no-op.
/// If p is non-null, it must have been created by
/// new_wasmtime_global_context and not previously freed.
#[no_mangle]
pub unsafe extern "C" fn free_wasmtime_global_context(p: *mut c_void) {
    if p.is_null() {
        return;
    }
    let context: *mut Arc<WasmtimeGlobalContext> =
        p.cast::<Arc<WasmtimeGlobalContext>>();
    unsafe { drop(Box::from_raw(context)) };
}
