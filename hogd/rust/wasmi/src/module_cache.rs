use lru::LruCache;

use wasmi::{Linker, Module};

use crate::SHA256Hash;
use core::ffi::c_void;
use std::num::NonZeroUsize;

#[derive(Clone)]
pub struct ModuleCacheEntry {
    pub module: Module,
    pub linker: Linker<*mut c_void>,
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
