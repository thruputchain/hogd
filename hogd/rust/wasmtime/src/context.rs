use core::ffi::c_void;
use core::slice;
use std::sync::Arc;

use crate::debug_log;

use wasmtime::{Engine, InstancePre, Linker, Module};

use crate::SHA256Hash;
pub use common::contract_loader::ContractGetFn;
pub use common::contract_loader::ContractGetResult;
use common::SystemErrorCode;

use crate::global_context::WasmtimeGlobalContext;
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

/// A Wasmtime runtime context that can be reused across multiple transactions.
///
/// This context is **not thread-safe** and must be used by a single thread at a time.
/// However, it can be reused across multiple sequential transactions within the same thread,
/// which provides better performance than creating a new context for each transaction.
pub struct WasmtimeContext {
    global_context: Arc<WasmtimeGlobalContext>,
    contract_get_fn: ContractGetFn,
    engine_index: usize,
}

impl WasmtimeContext {
    fn new(
        global_context: &Arc<WasmtimeGlobalContext>,
        get_fn: ContractGetFn,
    ) -> WasmtimeContext {
        let engine_index = global_context.alloc_engine_index();
        Self {
            global_context: Arc::clone(global_context),
            contract_get_fn: get_fn,
            engine_index,
        }
    }

    /// # Safety
    /// user_context must be a valid pointer to a HostCallContext
    pub unsafe fn fetch_contract<'a>(
        &self,
        user_context: *mut c_void,
        hash: &'a SHA256Hash,
    ) -> Result<Option<&'a [u8]>, SystemErrorCode> {
        debug_assert!(!user_context.is_null());
        let get_res =
            unsafe { (self.contract_get_fn)(user_context, hash.as_ptr()) };
        // Safety: the returned slice's lifetime is tied to 'a (the hash's lifetime),
        // which is bounded by the caller's transaction scope.
        unsafe { get_res.to_data() }
    }

    pub fn engine(&self) -> &Engine {
        self.global_context.engine(self.engine_index)
    }

    fn linker(&self) -> &Linker<*mut c_void> {
        self.global_context.linker(self.engine_index)
    }

    /// # Safety
    /// user_context must be a valid pointer to a HostCallContext
    pub unsafe fn get_instance_pre(
        &mut self,
        hash: &[u8; 32],
        user_context: *mut c_void,
    ) -> Result<Option<Arc<InstancePre<*mut c_void>>>, SystemErrorCode> {
        // Validate engine_index
        if self.engine_index >= self.global_context.engine_count() {
            return Err(SystemErrorCode::kSystemUnrecoverable);
        }
        // Check cache for this engine's InstancePre
        if let Some(instance_pre) = self
            .global_context
            .instance_pre_cache()
            .get(hash, self.engine_index)
        {
            return Ok(Some(instance_pre));
        }
        // otherwise compile the module and cache it for this engine
        let bytes = unsafe { self.fetch_contract(user_context, hash)? };
        if bytes.is_none() {
            return Ok(None);
        }
        let bytes = bytes.unwrap();
        let engine = self.engine();
        let module = Module::new(engine, bytes)
            .inspect_err(|_e| debug_log!(eprintln!("Module new error: {_e}")))
            .map_err(|_e| SystemErrorCode::kSystemUnrecoverable)?;

        if !check_memory_limit(&module) {
            debug_log!(eprintln!("Module instantiation failed: Memory export verification failed"));
            return Ok(None);
        }

        // The wasmtime API doesn't expose a way to check if a module has a start function.
        // Instead, if the module has a start function, it will fail to instantiate a new runtime
        // as we supply no fuel during instantiation.

        let instance_pre =
            Arc::new(match self.linker().instantiate_pre(&module) {
                Ok(instance_pre) => instance_pre,
                Err(e) => {
                    debug_log!(eprintln!("Error instantiating store: {e}"));
                    // Unknown import: module references a name the linker
                    // doesn't have.  Deterministic contract rejection.
                    if e.downcast_ref::<wasmtime::UnknownImportError>()
                        .is_some()
                    {
                        return Ok(None);
                    }
                    // Import type mismatch: the linker has a definition for
                    // the import name, but the module declares it as a
                    // different entity kind or with an incompatible
                    // signature.  Deterministic contract rejection.
                    //
                    // WASMTIME-VERSION-SENSITIVE (v42.0.0): this prefix is
                    // emitted by wasmtime/src/runtime/instance.rs::typecheck()
                    // via .with_context(|| format!("incompatible import type
                    // for `{name}::{field}`")).  There is no typed error for
                    // this case; re-verify this string on wasmtime upgrades.
                    const INCOMPATIBLE_IMPORT_MSG: &str =
                        "incompatible import type";
                    let msg = e.to_string();
                    if msg.contains(INCOMPATIBLE_IMPORT_MSG) {
                        return Ok(None);
                    }
                    debug_log!(eprintln!(
                        "Unrecoverable: Unknown error instantiating store: {e}"
                    ));
                    return Err(SystemErrorCode::kSystemUnrecoverable);
                }
            });

        self.global_context.instance_pre_cache().put(
            hash,
            self.engine_index,
            instance_pre.clone(),
        );

        Ok(Some(instance_pre))
    }

    pub fn verify_contract(&self, contract_data: &[u8]) -> bool {
        let engine = self.engine();
        let module = match Module::new(engine, contract_data) {
            Ok(m) => m,
            Err(e) => {
                #[cfg(debug_assertions)]
                eprintln!("Contract verification failed: {}", e);
                return false;
            }
        };
        if !check_memory_limit(&module) {
            #[cfg(debug_assertions)]
            eprintln!(
                "Contract verification failed: memory limit check failed"
            );
            return false;
        }
        let config = groundhog_wasm_meter::Config {
            max_imported_functions: 1,
            reject_floats: true,
            ..groundhog_wasm_meter::Config::default()
        };
        if let Err(e) = groundhog_wasm_meter::verify(contract_data, &config) {
            #[cfg(debug_assertions)]
            eprintln!("Contract verification failed: wasm-meter: {e}");
            return false;
        }
        true
    }
}

// Rust FFI needs no_mangle and extern "C"
/// # Safety
/// global_context must not be nullptr and must have been created by new_wasmtime_global_context.
/// module_cache can be nullptr, in which case no module cache is used.
/// module_cache must outlive WasmtimeContext.
/// Engine index is allocated automatically using a thread-local atomic pattern.
#[no_mangle]
pub unsafe extern "C" fn new_wasmtime_context(
    get_fn: ContractGetFn,
    global_context: *mut c_void,
) -> *mut c_void {
    debug_assert!(!global_context.is_null());

    let global_context: &Arc<WasmtimeGlobalContext> =
        unsafe { &*global_context.cast::<Arc<WasmtimeGlobalContext>>() };

    let b = Box::new(WasmtimeContext::new(global_context, get_fn));
    Box::into_raw(b).cast::<c_void>()
}

/// # Safety
/// If p is null, this function is a no-op.
/// If p is non-null, caller must have exclusive (non-concurrent) access,
/// p must have been created by new_wasmtime_context, and context must not be
/// accessed after this call.
#[no_mangle]
pub unsafe extern "C" fn free_wasmtime_context(p: *mut c_void) {
    if p.is_null() {
        return;
    }
    let context: *mut WasmtimeContext = p.cast::<WasmtimeContext>();
    unsafe { drop(Box::from_raw(context)) };
}

/// # Safety
/// Context must not be a nullptr.
/// Context must have been created by new_wasmtime_context.
/// Multiple contracts can be verified concurrently.
#[no_mangle]
pub unsafe extern "C" fn wasmtime_verify_contract(
    p: *const c_void,
    contract_data_p: *const u8,
    contract_size: usize,
) -> bool {
    debug_assert!(!p.is_null());
    if contract_data_p.is_null() {
        #[cfg(debug_assertions)]
        eprintln!("Contract verification failed: null contract data pointer");
        return false;
    }
    let context: *const WasmtimeContext = p.cast::<WasmtimeContext>();
    let wasm: &[u8] =
        unsafe { slice::from_raw_parts(contract_data_p, contract_size) };
    unsafe { (*context).verify_contract(wasm) }
}
