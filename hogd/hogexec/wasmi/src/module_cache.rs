use lru::LruCache;

use wasmi::Module;

use core::ffi::c_void;
use std::num::NonZeroUsize;
use std::sync::{Arc, Mutex};

type SHA256Hash = [u8; 32];

pub struct ModuleCache {
    cache: Mutex<LruCache<SHA256Hash, Module>>,
}

pub type ModuleCacheRef = Arc<ModuleCache>;

impl ModuleCache {
    pub fn new(capacity: usize) -> Self {
        Self {
            cache: Mutex::new(LruCache::new(
                NonZeroUsize::new(capacity).unwrap(),
            )),
        }
    }

    pub fn get(&self, hash: &SHA256Hash) -> Option<Module> {
        self.cache.lock().unwrap().get(hash).cloned()
    }

    pub fn put(&self, hash: &SHA256Hash, module: &Module) {
        self.cache.lock().unwrap().put(*hash, module.clone());
    }
}

/// # Safety
/// capacity must be nonzero.
#[no_mangle]
pub unsafe extern "C" fn new_wasmi_module_cache(
    capacity: usize,
) -> *mut c_void {
    let b = Box::new(Arc::new(ModuleCache::new(capacity)));
    let ptr = Box::into_raw(b);
    core::mem::transmute(ptr)
}

/// # Safety
/// ptr must be a pointer to an ModuleCacheRef (Arc<ModuleCache>), which was produced by new_wasmi_module_cache.
#[no_mangle]
pub unsafe extern "C" fn free_wasmi_module_cache(ptr: *mut c_void) {
    unsafe {
        drop(Box::from_raw(ptr as *mut ModuleCacheRef));
    }
}
