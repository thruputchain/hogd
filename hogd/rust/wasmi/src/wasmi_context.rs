use wasmi::core::ValType;
use wasmi::{
    Caller, CompilationMode, Config, Engine, Error, ExternType, Linker, Module,
    StackLimits,
};

use core::ffi::c_void;
use core::slice;

use crate::module_cache::{ModuleCache, ModuleCacheEntry};
use crate::wasmi_debug_log as debug_log;
use crate::wasmi_error::TrampolineError;
use crate::wasmi_runtime::check_memory_limit;
use crate::SHA256Hash;
pub use common::contract_loader::ContractGetFn;
pub use common::contract_loader::ContractGetResult;
use common::string_from_parts;
use common::{
    InvokeErrorCode, LinearMemorySlice, LinkEntry, SystemErrorCode, WasmContext,
};

// A WasmiContext provides shared data structures
// for multiple Wasmi runtimes owned by one RuntimeContext.
// This structure is not threadsafe; hogexec keeps RuntimeContext
// thread-confined.
// Specifically, Engine is a shared_pointer around EngineInner,
// which appears to have appropriate mutexes.
//
// One strategy would be to create a shared Engine object, and then put each Module
// as a (cached) object on the Engine.  This would save some module instantiation costs.
// This is what stellar-core does
// https://github.com/stellar/rs-soroban-env/blob/0a1c157deb1214f963a4e3dbc9abdce8ab5ef84a/soroban-env-host/src/vm/module_cache.rs#L87
//
// The memory usage savings of a shared Engine are nonzero (we save when one Module is used by many transactions).
// By size, (I suspect) the biggest costs are the the Wasmi codemap and the instance stacks
// https://docs.rs/wasmi/latest/src/wasmi/engine/mod.rs.html#429
// The codemap is what stores compiled wasm functions -- this could be deduplicated across shared modules.
// The instance stacks store the wasm stack, which must be distinct for every instance (so no savings possible there).
//
// However, there are two problems with the current Wasmi API.
// (1) There is an internal lock on the recycled Stacks object inside the engine,
// which, in effect, prevents any concurrent execution on one engine.  This is likely fixable
// by a slight API change.
//
// (2) Entries in the CodeMap are never garbage collected.  That means that if an engine is reused across transactions,
// without a module cache, we will leak (nontrivial amounts of) memory.  This also means that, for example,
// a cache of modules cannot be effectively implemented on a shared engine.
//
// Each transaction requires its own Store.  It would be nice if we could share Modules and InstancePres
// across transactions, but that's not viable -- Module requires an Engine parameter (we could share engines, in principle)
// and InstancePre requires a Store parameter (unlike, for example, the wasmtime api).
//
// This implementation borrows some of the wasmi use patterns from stellar
// e.g. https://github.com/stellar/rs-soroban-env/blob/f9a51971d3ca66216e290b074a1feee5334b58ff/soroban-env-host/src/vm.rs#L166
//
// The WASM official C API spec says that only one Engine should be created
// per process, although that's more strict than necessary.
// Linker can be shared between instances.
// Store and Module and Instance are per-WasmiRuntime structures.

// WARNING (Especially if we use a Module cache):  Wasmi charges gas for lazy compilation and validation of modules,
// but not for eager compilation and validation (unclear why this choice was made).  If a module cache is used,
// we must use CompilationMode::Eager.

#[derive(Default)]
pub struct WasmiGlobalContext {
    entries: Vec<LinkEntry>,
}

unsafe impl Send for WasmiGlobalContext {}
unsafe impl Sync for WasmiGlobalContext {}

impl WasmiGlobalContext {
    pub fn add_entry(&mut self, entry: LinkEntry) {
        self.entries.push(entry);
    }

    pub fn entries(&self) -> &Vec<LinkEntry> {
        &self.entries
    }
}

fn validate_imports(module: &Module, entries: &[LinkEntry]) -> bool {
    let manifest: std::collections::HashMap<(&str, &str), u8> = entries
        .iter()
        .map(|e| ((e.module(), e.function()), e.nargs()))
        .collect();

    for import in module.imports() {
        let key = (import.module(), import.name());
        let expected_nargs = match manifest.get(&key) {
            Some(n) => *n as usize,
            None => {
                eprintln!(
                    "wasmi: rejecting contract — unknown host import {}::{} (likely SDK/runtime ABI mismatch)",
                    import.module(),
                    import.name()
                );
                return false;
            }
        };
        let func_ty = match import.ty() {
            ExternType::Func(f) => f,
            other => {
                eprintln!(
                    "wasmi: rejecting contract — import {}::{} declared as non-function: {other:?}",
                    import.module(),
                    import.name()
                );
                return false;
            }
        };
        let params = func_ty.params();
        let results = func_ty.results();
        if params.len() != expected_nargs {
            eprintln!(
                "wasmi: rejecting contract — import {}::{} arity mismatch: contract declares {} params, host registers {} (likely SDK/runtime ABI mismatch)",
                import.module(),
                import.name(),
                params.len(),
                expected_nargs
            );
            return false;
        }
        if !params.iter().all(|p| *p == ValType::I64) {
            eprintln!(
                "wasmi: rejecting contract — import {}::{} param-type mismatch: contract types {params:?}, host requires all-i64",
                import.module(),
                import.name()
            );
            return false;
        }
        if results.len() != 1 || results[0] != ValType::I32 {
            eprintln!(
                "wasmi: rejecting contract — import {}::{} result-type mismatch: contract results {results:?}, host requires single i32",
                import.module(),
                import.name()
            );
            return false;
        }
    }
    true
}

pub struct WasmiContext {
    global_context: std::sync::Arc<WasmiGlobalContext>,
    // ModuleCache itself is !Sync because LRU lookup updates access
    // order. Hogexec keeps WasmiContext thread-confined, so runtime
    // creation uses an exclusive borrow instead of synchronizing the
    // cache internally.
    pub module_cache: Option<ModuleCache>,
    contract_get_fn: ContractGetFn,
}

fn get_memory_export(
    caller: &mut Caller<'_, *mut c_void>,
) -> Option<wasmi::Memory> {
    caller.get_export("memory")?.into_memory()
}

fn get_linear_memory_slice(
    caller: &mut Caller<'_, *mut c_void>,
) -> LinearMemorySlice {
    if let Some(memory) = get_memory_export(caller) {
        let slice = memory.data_mut(caller);
        LinearMemorySlice {
            mem: slice.as_mut_ptr(),
            sz: slice.len() as u64,
        }
    } else {
        LinearMemorySlice {
            mem: std::ptr::null_mut(),
            sz: 0,
        }
    }
}

macro_rules! link_function_nargs {
    ($name:ident, $cfunc:ident $(, $arg:ident)*) => {
        fn $name(
            linker: &mut Linker<*mut c_void>,
            fn_pointer: common::ErasedFnPtr,
            import_name: &str,
            fn_name: &str,
        ) -> Result<(), Error> {
            let ptr = fn_pointer;
            linker
                .func_wrap(import_name, fn_name, move |mut caller: Caller<'_, *mut c_void>, $($arg: u64),*| -> Result<i32, wasmi::Error> {

                    let gas_remaining = match caller.get_fuel() {
                        Ok(fuel) => fuel,
                        Err(_e) => {
                            debug_log!(eprintln!("Unrecoverable: failed to get fuel: {_e:?}"));
                            return Err(Error::host(TrampolineError { error: common::HostFnErrorCode::Unrecoverable }));
                        }
                    };

                    let wasm_context : WasmContext = WasmContext {
                        memory: get_linear_memory_slice(&mut caller),
                        gas_remaining,
                    };

                    let (res, gas_consumed) : (Result<i32, common::HostFnErrorCode>, u64) = {
                        common::$cfunc(
                            ptr,
                            *caller.data(),
                            wasm_context,
                            $($arg),*
                        )
                    }.into();

                    assert!(gas_consumed <= gas_remaining);
                    caller.set_fuel(gas_remaining - gas_consumed).unwrap();

                    res.map_err(|error| Error::host(TrampolineError { error }))
                })
                .map_err(Into::into)
                .map(|_| ())
        }
    };
}

impl WasmiContext {
    fn new(
        global_context: std::sync::Arc<WasmiGlobalContext>,
        get_fn: ContractGetFn,
    ) -> WasmiContext {
        Self {
            global_context,
            // Performance is somewhat sensitive to module cache size.
            // We need to change the caching strategy anyways once we go to winch.
            module_cache: Some(ModuleCache::new(10)),
            contract_get_fn: get_fn,
        }
    }

    /// # Safety
    /// user_context must be a valid pointer to a HostCallContext (or a nullptr).
    /// The lifetime of the returned contract is tied to the lifetime of the `hash` parameter,
    /// ensuring the slice doesn't outlive the calling function scope.
    pub unsafe fn fetch_contract<'a>(
        &self,
        user_context: *mut c_void,
        hash: &'a SHA256Hash,
    ) -> Result<Option<&'a [u8]>, SystemErrorCode> {
        let get_res =
            unsafe { (self.contract_get_fn)(user_context, hash.as_ptr()) };
        // Safety: the returned slice's lifetime is tied to 'a (the hash's lifetime),
        // which is bounded by the caller's transaction scope.
        unsafe { get_res.to_data() }
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

    fn new_engine() -> Engine {
        Engine::new(
            Config::default()
                .consume_fuel(true)
                .floats(false)
                .set_stack_limits(StackLimits::default())
                // MUST BE EAGER
                // This warning is in ALL CAPITALS because if we change that
                // we will have NONDETERMINISM in gas measurements
                // and THAT WOULD BE BAD if we have the module cache enabled
                .compilation_mode(CompilationMode::Eager),
        )
        // WARNING:
        // Caching is INCOMPATIBLE with wasmi's lazy compilation.
        // If we upgrade to later wasmi versions, we should disable
        // simd support (relaxed_simd at least, is apparently not deterministic)
        // .wasm_simd(false)
        // .wasm_relaxed_simd(false),
    }

    pub fn linker(&self) -> Linker<*mut c_void> {
        let engine = Self::new_engine();
        let mut linker = Linker::new(&engine);

        let entries = self.global_context.entries();
        for entry in entries {
            match entry.nargs() {
                0 => Self::link_function_0args(
                    &mut linker,
                    entry.fn_pointer(),
                    entry.module(),
                    entry.function(),
                ),
                1 => Self::link_function_1args(
                    &mut linker,
                    entry.fn_pointer(),
                    entry.module(),
                    entry.function(),
                ),
                2 => Self::link_function_2args(
                    &mut linker,
                    entry.fn_pointer(),
                    entry.module(),
                    entry.function(),
                ),
                3 => Self::link_function_3args(
                    &mut linker,
                    entry.fn_pointer(),
                    entry.module(),
                    entry.function(),
                ),
                4 => Self::link_function_4args(
                    &mut linker,
                    entry.fn_pointer(),
                    entry.module(),
                    entry.function(),
                ),
                5 => Self::link_function_5args(
                    &mut linker,
                    entry.fn_pointer(),
                    entry.module(),
                    entry.function(),
                ),
                6 => Self::link_function_6args(
                    &mut linker,
                    entry.fn_pointer(),
                    entry.module(),
                    entry.function(),
                ),
                7 => Self::link_function_7args(
                    &mut linker,
                    entry.fn_pointer(),
                    entry.module(),
                    entry.function(),
                ),
                8 => Self::link_function_8args(
                    &mut linker,
                    entry.fn_pointer(),
                    entry.module(),
                    entry.function(),
                ),
                9 => Self::link_function_9args(
                    &mut linker,
                    entry.fn_pointer(),
                    entry.module(),
                    entry.function(),
                ),
                10 => Self::link_function_10args(
                    &mut linker,
                    entry.fn_pointer(),
                    entry.module(),
                    entry.function(),
                ),
                11 => Self::link_function_11args(
                    &mut linker,
                    entry.fn_pointer(),
                    entry.module(),
                    entry.function(),
                ),
                12 => Self::link_function_12args(
                    &mut linker,
                    entry.fn_pointer(),
                    entry.module(),
                    entry.function(),
                ),
                _ => {
                    panic!(
                        "Unsupported number of arguments: {}",
                        entry.nargs()
                    );
                }
            }
            .unwrap();
        }

        linker
    }

    pub fn verify_contract(&self, contract_data: &[u8]) -> InvokeErrorCode {
        let config = groundhog_wasm_meter::Config {
            max_imported_functions: 1,
            reject_floats: true,
            ..groundhog_wasm_meter::Config::default()
        };
        let module_info =
            match groundhog_wasm_meter::verify(contract_data, &config) {
                Ok(info) => info,
                Err(e) => {
                    #[cfg(debug_assertions)]
                    eprintln!("Contract verification failed: wasm-meter: {e}");
                    return match e {
                    groundhog_wasm_meter::Error::InternalResourceExhausted(
                        _,
                    ) => InvokeErrorCode::Unrecoverable,
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
        let engine = Self::new_engine();
        let module = match Module::new(&engine, contract_data) {
            Ok(m) => m,
            Err(e) => {
                #[cfg(debug_assertions)]
                eprintln!("Contract verification failed: {}", e);
                return InvokeErrorCode::Deterministic;
            }
        };
        if !check_memory_limit(&module) {
            #[cfg(debug_assertions)]
            eprintln!(
                "Contract verification failed: memory limit check failed"
            );
            return InvokeErrorCode::Deterministic;
        }
        if !validate_imports(&module, self.global_context.entries()) {
            #[cfg(debug_assertions)]
            eprintln!("Contract verification failed: import validation failed");
            return InvokeErrorCode::Deterministic;
        }
        InvokeErrorCode::None
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
/// Caller must have exclusive (non-concurrent) access to the context.
/// context_void must not be a nullptr.
/// Context must have been created by new_wasmi_context.
#[no_mangle]
pub unsafe extern "C" fn wasmi_link_nargs(
    context_void: *mut c_void,
    module_name: *const u8,
    module_name_len: u32,
    method_name: *const u8,
    method_name_len: u32,
    function_pointer: common::ErasedFnPtr,
    nargs: u8,
) -> bool // true if success
{
    debug_assert!(!context_void.is_null());
    debug_assert!(!module_name.is_null());
    debug_assert!(!method_name.is_null());

    let context: &mut WasmiContext =
        unsafe { &mut *context_void.cast::<WasmiContext>() };

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

    // Directly mutate via Arc::get_mut on the legitimately mutable reference,
    // avoiding the aliasing UB of laundering &Arc -> *mut -> &mut Arc.
    if let Some(global_context_mut) =
        std::sync::Arc::get_mut(&mut context.global_context)
    {
        global_context_mut.add_entry(LinkEntry::new(
            module,
            method,
            nargs,
            function_pointer,
        ));
        true
    } else {
        panic!(
            "Unrecoverable: Multiple references exist to global context in wasmi_link_nargs"
        );
    }
}

/// # Safety
/// Global context must not be a nullptr.
/// Global context must have been created by new_wasmi_global_context.
#[no_mangle]
pub unsafe extern "C" fn wasmi_global_link_nargs(
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

    let global_context: &mut std::sync::Arc<WasmiGlobalContext> = unsafe {
        &mut *global_context_void.cast::<std::sync::Arc<WasmiGlobalContext>>()
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
    if let Some(global_context_mut) = std::sync::Arc::get_mut(global_context) {
        global_context_mut.add_entry(LinkEntry::new(
            module,
            method,
            nargs,
            function_pointer,
        ));
        true
    } else {
        panic!(
            "Unrecoverable: Multiple references exist to global context in wasmi_global_link_nargs"
        );
    }
}

/// # Safety
/// Creates a global context that can be shared across threads.
/// Global context is threadsafe.
#[no_mangle]
pub unsafe extern "C" fn new_wasmi_global_context() -> *mut c_void {
    let global_context = std::sync::Arc::new(WasmiGlobalContext::default());

    Box::into_raw(Box::new(global_context)).cast::<c_void>()
}

/// # Safety
/// If p is null, this function is a no-op.
/// If p is non-null, it must have been created by new_wasmi_global_context
/// and not previously freed.
#[no_mangle]
pub unsafe extern "C" fn free_wasmi_global_context(p: *mut c_void) {
    if p.is_null() {
        return;
    }
    let global_context: *mut std::sync::Arc<WasmiGlobalContext> =
        p.cast::<std::sync::Arc<WasmiGlobalContext>>();
    unsafe { drop(Box::from_raw(global_context)) };
}

// Rust FFI needs no_mangle and extern "C"
/// # Safety
/// get_fn must not be a nullptr.
/// global_context must not be a nullptr and must have been created by new_wasmi_global_context.
#[no_mangle]
pub unsafe extern "C" fn new_wasmi_context(
    get_fn: ContractGetFn,
    global_context: *mut c_void,
) -> *mut c_void {
    debug_assert!(!global_context.is_null());

    let global_context: &std::sync::Arc<WasmiGlobalContext> = unsafe {
        &*global_context.cast::<std::sync::Arc<WasmiGlobalContext>>()
    };

    let b = Box::new(WasmiContext::new(global_context.clone(), get_fn));
    Box::into_raw(b).cast::<c_void>()
}

/// # Safety
/// If p is null, this function is a no-op.
/// If p is non-null, caller must have exclusive (non-concurrent) access,
/// p must have been created by new_wasmi_context, and context must not be
/// accessed after this call.
#[no_mangle]
pub unsafe extern "C" fn free_wasmi_context(p: *mut c_void) {
    if p.is_null() {
        return;
    }
    let context: *mut WasmiContext = p.cast::<WasmiContext>();
    unsafe { drop(Box::from_raw(context)) };
}

/// # Safety
/// Context must not be a nullptr.
/// Context must have been created by new_wasmi_context.
/// Multiple contracts can be verified concurrently.
/// The linker can be modified concurrently.
#[no_mangle]
pub unsafe extern "C" fn wasmi_verify_contract(
    p: *const c_void,
    contract_data_p: *const u8,
    contract_size: usize,
) -> i32 {
    debug_assert!(!p.is_null());
    if contract_data_p.is_null() {
        #[cfg(debug_assertions)]
        eprintln!("Contract verification failed: null contract data pointer");
        return InvokeErrorCode::Deterministic.0;
    }
    let context: *const WasmiContext = p.cast::<WasmiContext>();
    let wasm: &[u8] =
        unsafe { slice::from_raw_parts(contract_data_p, contract_size) };
    unsafe { (*context).verify_contract(wasm) }.0
}

// ===========================================================================
// Regression tests for audit issue #1522 (H001):
// Float rejection at the wasmi validation layer.
//
// The live wasmtime/Winch path was the primary subject of #1522, but the
// dormant wasmi backend must hold the same invariant so a future backend
// swap can't silently re-expose non-canonical NaN semantics. wasmi's
// Config::floats(false) threads the equivalent of wasmparser's FLOATS
// feature flag, causing Module::new to reject f32/f64 operators and types
// at validation — independent of (and redundant with) the separate
// groundhog_wasm_meter reject_floats check in verify_contract.
// ===========================================================================
#[cfg(test)]
mod h001_float_rejection_tests {
    use super::*;

    // Minimal float-bearing module — exercises both a float type (f32
    // return) and a float operator (f32.const). Either triggers wasmi's
    // validation gate when the engine has floats disabled.
    fn float_module_bytes() -> Vec<u8> {
        let wat = r#"
            (module
              (func (export "f") (result f32)
                (f32.const 0.0))
              (memory 1 1)
              (export "memory" (memory 0)))
        "#;
        wat::parse_str(wat).expect("inline float WAT must parse")
    }

    // Anchors the production engine's .floats(false) to validation
    // behavior. If anyone drops the call from new_engine, Module::new
    // would accept float-bearing modules (wasmi would compile them with
    // host-native NaN semantics) and this assertion catches the
    // regression.
    #[test]
    fn production_engine_rejects_float_module_at_validation() {
        let engine = WasmiContext::new_engine();
        let bytes = float_module_bytes();
        let err = Module::new(&engine, &bytes).err().expect(
            "wasmi engine built with .floats(false) must reject \
             float-bearing modules at validation",
        );
        let msg = format!("{err:#}");
        assert!(
            msg.contains("floating-point") || msg.contains("float"),
            "expected wasmi validation rejection mentioning floats, got: \
             {err:#}"
        );
    }

    // End-to-end via the public verify_contract entry point. Either the
    // wasmi-level .floats(false) gate (Module::new) or the
    // groundhog_wasm_meter::verify reject_floats gate will reject —
    // verify_contract returning a non-None error is the only behavior
    // callers observe, and that's what consensus depends on.
    #[test]
    fn verify_contract_rejects_float_module() {
        // Stub contract_get_fn — verify_contract doesn't touch it, but
        // WasmiContext::new requires a function pointer.
        unsafe extern "C" fn stub_get_fn(
            _userctx: *mut c_void,
            _hash: *const u8,
        ) -> ContractGetResult {
            ContractGetResult::new(SystemErrorCode::Unrecoverable.0, &[])
        }

        let global_context = std::sync::Arc::new(WasmiGlobalContext::default());
        let ctx = WasmiContext::new(global_context, stub_get_fn);
        let bytes = float_module_bytes();
        assert_ne!(
            ctx.verify_contract(&bytes),
            InvokeErrorCode::None,
            "wasmi verify_contract must reject float-bearing modules"
        );
    }
}
