use wasmi::{Linker, Module};

use common::SystemErrorCode;
use core::ffi::c_void;

use crate::debug_log;
type SHA256Hash = [u8; 32];

#[derive(Clone)]
pub struct ModuleCacheEntry {
    pub module: Module,
    pub linker: Linker<*mut c_void>,
}

// The external module cache entry is not just a *ModuleCacheEntry, but actually
// a std::shared_ptr<*mut c_void> that contains the ModuleCacheEntry.
// We need some callback to release the reference held in the shared_ptr on the c++ side when the object is dropped.
#[repr(C)]
#[derive(Clone)]
pub struct ModuleCacheGetResult {
    pub entry: *mut c_void,
    pub free_context: *mut c_void,
}

// input: cache ptr: input: SHA256Hash, output: ModuleCacheGetResult
pub type ModuleCacheGetFn =
    unsafe extern "C" fn(*mut c_void, *const u8) -> ModuleCacheGetResult;
// input: cache ptr: input: SHA256Hash, input: ModuleCacheEntry, output: SystemErrorCode
pub type ModuleCachePutFn =
    unsafe extern "C" fn(*mut c_void, *const u8, *mut c_void) -> i32;
// input: ModuleCacheGetResult.free_context ptr
pub type ModuleCacheEntryPtrFreeFn = unsafe extern "C" fn(*mut c_void) -> ();

/// # Safety
/// entry_ptr must be a pointer to an ModuleCacheEntry
#[no_mangle]
pub unsafe extern "C" fn free_module_cache_entry(entry_ptr: *mut c_void) {
    drop(Box::from_raw(entry_ptr as *mut ModuleCacheEntry));
}

pub struct ModuleCache {
    pub cache: *mut c_void,
    get_fn: ModuleCacheGetFn,
    put_fn: ModuleCachePutFn,
    entry_ptr_free_fn: ModuleCacheEntryPtrFreeFn,
    module_refs: Vec<ModuleCacheGetResult>,
}

impl Drop for ModuleCache {
    fn drop(&mut self) {
        for r in self.module_refs.iter() {
            unsafe {
                (self.entry_ptr_free_fn)(r.free_context);
            }
        }
    }
}

impl ModuleCache {
    pub fn new(
        cache: *mut c_void,
        get_fn: ModuleCacheGetFn,
        put_fn: ModuleCachePutFn,
        entry_ptr_free_fn: ModuleCacheEntryPtrFreeFn,
    ) -> Self {
        Self {
            cache,
            get_fn,
            put_fn,
            entry_ptr_free_fn,
            module_refs: Vec::new(),
        }
    }

    pub fn get(&mut self, hash: &SHA256Hash) -> Option<ModuleCacheEntry> {
        let res: ModuleCacheGetResult =
            unsafe { (self.get_fn)(self.cache, hash.as_ptr()) };

        if res.entry.is_null() {
            None
        } else {
            self.module_refs.push(res.clone());
            Some(unsafe { &*(res.entry as *mut ModuleCacheEntry) }.clone())
        }
    }

    pub fn put(
        &mut self,
        hash: &SHA256Hash,
        module: &Module,
        linker: &Linker<*mut c_void>,
    ) -> Result<(), SystemErrorCode> {
        let entry = Box::new(ModuleCacheEntry {
            module: module.clone(),
            linker: linker.clone(),
        });
        unsafe {
            let res = (self.put_fn)(
                self.cache,
                hash.as_ptr(),
                Box::into_raw(entry) as *mut c_void,
            );
            if res == 0 {
                return Ok(());
            }

            let error: Result<SystemErrorCode, ()> = res.try_into();

            if let Ok(error) = error {
                return Err(error);
            }
            debug_log!(eprintln!("Unknown system error code: {res}"));
            Err(SystemErrorCode::kSystemUnrecoverable)
        }
    }
}
