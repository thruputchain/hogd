use lru::LruCache;

use wasmtime::InstancePre;

use crate::SHA256Hash;
use core::ffi::c_void;
use std::num::NonZeroUsize;

#[derive(Clone)]
pub struct ModuleCacheEntry {
    pub instance: InstancePre<*mut c_void>,
}

// Module cache is NOT threadsafe, and should NOT be shared across threads.
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
        instance: &InstancePre<*mut c_void>,
    ) {
        self.cache.put(
            *hash,
            ModuleCacheEntry {
                instance: instance.clone(),
            },
        );
    }

    pub fn cached_size(&self) -> usize {
        self.cache.len()
    }
}

/// # Safety
/// capacity must be nonzero.
#[no_mangle]
pub unsafe extern "C" fn new_wasmtime_module_cache(
    capacity: usize,
) -> *mut c_void {
    let b = Box::new(ModuleCache::new(capacity));
    let ptr = Box::into_raw(b);
    core::mem::transmute(ptr)
}

/// # Safety
/// ptr must be a pointer to a ModuleCache, which was produced by new_wasmtime_module_cache.
#[no_mangle]
pub unsafe extern "C" fn free_wasmtime_module_cache(ptr: *mut c_void) {
    debug_assert!(!ptr.is_null());
    unsafe {
        drop(Box::from_raw(ptr as *mut ModuleCache));
    }
}

/// # Safety
/// ptr must be a pointer to a ModuleCache, which was produced by new_wasmtime_module_cache.
#[no_mangle]
pub unsafe extern "C" fn wasmtime_module_cache_cached_size(
    ptr: *mut c_void,
) -> usize {
    debug_assert!(!ptr.is_null());
    let cache: *mut ModuleCache = unsafe { core::mem::transmute(ptr) };
    let cache = unsafe { &mut *cache };
    cache.cached_size()
}
