use lru::LruCache;

use wasmi::{Linker, Module};

use core::ffi::c_void;
use std::num::NonZeroUsize;

type SHA256Hash = [u8; 32];

#[derive(Clone)]
pub struct ModuleCacheEntry {
    pub module: Module,
    pub linker: Linker<*mut c_void>,
}

// Module cache is NOT threadsafe, and shoult NOT be shared across threads.
pub struct ModuleCache {
    cache: LruCache<SHA256Hash, ModuleCacheEntry>,
}

impl ModuleCache {
    pub fn new(capacity: usize) -> Self {
        Self {
            cache: LruCache::new(NonZeroUsize::new(capacity).unwrap()),
        }
    }

    pub fn get(&mut self, hash: &SHA256Hash) -> Option<ModuleCacheEntry> {
        self.cache.get(hash).cloned()
    }

    pub fn put(
        &mut self,
        hash: &SHA256Hash,
        module: &Module,
        linker: &Linker<*mut c_void>,
    ) {
        self.cache.put(
            *hash,
            ModuleCacheEntry {
                module: module.clone(),
                linker: linker.clone(),
            },
        );
    }
}

/// # Safety
/// capacity must be nonzero.
#[no_mangle]
pub unsafe extern "C" fn new_wasmi_module_cache(
    capacity: usize,
) -> *mut c_void {
    let b = Box::new(ModuleCache::new(capacity));
    let ptr = Box::into_raw(b);
    core::mem::transmute(ptr)
}

/// # Safety
/// ptr must be a pointer to an ModuleCache, which was produced by new_wasmi_module_cache.
#[no_mangle]
pub unsafe extern "C" fn free_wasmi_module_cache(ptr: *mut c_void) {
    debug_assert!(!ptr.is_null());
    unsafe {
        drop(Box::from_raw(ptr as *mut ModuleCache));
    }
}
