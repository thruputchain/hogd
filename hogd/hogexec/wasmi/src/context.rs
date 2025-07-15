use wasmi::{Caller, Config, Engine, Error, Linker, Module, StackLimits};

use core::ffi::c_void;
use core::slice;
use core::str::Utf8Error;

use crate::external_call;
use crate::module_cache::{ModuleCache, ModuleCacheEntry};

// A WasmiContext provides shared data structures
// for multiple Wasmi runtimes.
// This structure internally (appears to be) threadsafe.
// Specifically, Engine is a shared_pointer around EngineInner,
// which appears to have appropriate mutexes.
//
// One strategy would be to create a shared Engine object, and then put each Module
// as a (cached) object on the Engine.  This would save some module instantiation costs.
// This is what stellar-core does
// https://github.com/stellar/rs-soroban-env/blob/0a1c157deb1214f963a4e3dbc9abdce8ab5ef84a/soroban-env-host/src/vm/module_cache.rs#L87
//
// However, the internal data structures of EngineInner have the potential to be contended,
// especially under Groundhog's concurrent execution model.
//
// The memory usage savings of a shared Engine are nonzero (we save when one Module is used by many transactions).
// By size, (I suspect) the biggest costs are the the Wasmi codemap and the instance stacks
// https://docs.rs/wasmi/latest/src/wasmi/engine/mod.rs.html#429
// The codemap is what stores compiled wasm functions -- this could be deduplicated across shared modules.
// The instance stacks store the wasm stack, which must be distinct for every instance (so no savings possible there).
//
// However, there is one big problem with the Wasmi API.  Unless I (geoff) am misreading wasmi,
// entries in the CodeMap are never garbage collected.  That means that if an engine is reused across transactions,
// without a module cache, we will leak (nontrivial amounts of) memory.  This also means that, for example,
// an LRU cache of modules cannot be implemented.
//
// Each transaction requires its own Store.  It would be nice if we could share Modules and InstancePres
// across transactions, but that's not viable -- Module requires an Engine parameter (we could share engines, in principle)
// and InstancePre requires a Store parameter (unlike, for example, the wasmtime api).
//
// This implementation borrows some of the wasmi use patterns from stellar (without the module cache)
// e.g. https://github.com/stellar/rs-soroban-env/blob/f9a51971d3ca66216e290b074a1feee5334b58ff/soroban-env-host/src/vm.rs#L166
//
// The WASM official C API spec says that only one Engine should be created
// per process, although that's more strict than necessary.
// Linker can be shared between instances.
// Store and Module and Instance are per-WasmiRuntime structures.

// WARNING (Especially if we use a Module cache):  Wasmi charges gas for lazy compilation and validation of modules,
// but not for eager compilation and validation (unclear why this choice was made).  If a module cache is used,
// we must use CompilationMode::Eager.

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

pub struct WasmiContext<'a> {
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

fn string_from_parts(bytes: *const u8, len: u32) -> Result<String, Utf8Error> {
    let slice = unsafe { slice::from_raw_parts(bytes, len as usize) };
    match std::str::from_utf8(slice) {
        Ok(s) => Ok(s.to_owned()), // TODO check this is a copy and not something weird
        Err(x) => Err(x),
    }
}

macro_rules! link_function_nargs {
    ($name:ident, $cfunc:ident $(, $arg:ident)*) => {
        fn $name(
            linker: &mut Linker<*mut c_void>,
            fn_pointer: *mut c_void,
            import_name: &str,
            fn_name: &str,
        ) -> Result<(), Error> {
            let x = BorrowBypass { fn_pointer };
            linker
                .func_wrap(import_name, fn_name, move |caller: Caller<'_, *mut c_void>, $($arg: u64),*| {
                    let res = unsafe {
                        external_call::$cfunc(
                            x.clone().fn_pointer,
                            *caller.data(),
                            $($arg),*
                        )
                    };
                    external_call::handle_trampoline_error(res)
                })
                .map_err(Into::into)
                .map(|_| ())
        }
    };
}

impl<'a> WasmiContext<'a> {
    fn new(module_cache: Option<&'a mut ModuleCache>) -> WasmiContext<'a> {
        Self {
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
        self.module_cache
            .as_mut()
            .map(|cache| cache.put(hash, module, linker));
    }

    pub fn add_entry(&mut self, entry: LinkEntry) {
        self.entries.push(entry);
    }

    fn new_engine(&self) -> Engine {
        Engine::new(
            Config::default()
                .consume_fuel(true)
                .floats(false)
                .set_stack_limits(StackLimits::default()),
        )
        // If we upgrade to later wasmi versions, we should disable
        // simd support (relaxed_simd at least, is apparently not deterministic)
        // .wasm_simd(false)
        // .wasm_relaxed_simd(false),
    }

    pub fn new_linker(&self) -> Linker<*mut c_void> {
        let engine = self.new_engine();

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
        let engine = self.new_engine();
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
pub unsafe extern "C" fn wasmi_link_nargs(
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
pub unsafe extern "C" fn new_wasmi_context(
    module_cache: *mut c_void,
) -> *mut c_void {
    let b = if module_cache.is_null() {
        Box::new(WasmiContext::new(None))
    } else {
        Box::new(WasmiContext::new(unsafe {
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
/// Context must have been created by new_wasmi_context.
/// Context must not be accessed after this call.
#[no_mangle]
pub unsafe extern "C" fn free_wasmi_context(p: *mut c_void) {
    debug_assert!(!p.is_null());

    let context: *mut WasmiContext = unsafe { core::mem::transmute(p) };

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
) -> bool {
    debug_assert!(!p.is_null());

    // empty contract might trigger this case.
    if contract_data_p.is_null() {
        return false;
    }

    let context: *const WasmiContext = unsafe { core::mem::transmute(p) };

    let wasm: &[u8] =
        unsafe { slice::from_raw_parts(contract_data_p, contract_size) };

    unsafe { (*context).verify_contract(wasm) }
}
