use synchronized_map::SynchronizedMap;

use crate::SHA256Hash;
use wasmtime::InstancePre;

use core::ffi::c_void;

use std::sync::Arc;

pub struct InstancePreCacheEntry {
    instance_pres: Vec<Option<Arc<InstancePre<*mut c_void>>>>,
}

impl InstancePreCacheEntry {
    pub fn new(engine_count: usize) -> Self {
        Self {
            instance_pres: vec![None; engine_count],
        }
    }

    pub fn get(
        &self,
        engine_index: usize,
    ) -> &Option<Arc<InstancePre<*mut c_void>>> {
        debug_assert!(engine_index < self.instance_pres.len());
        unsafe { self.instance_pres.get_unchecked(engine_index) }
    }

    pub fn put(
        &mut self,
        engine_index: usize,
        instance_pre: Arc<InstancePre<*mut c_void>>,
    ) {
        debug_assert!(engine_index < self.instance_pres.len());
        unsafe {
            *self.instance_pres.get_unchecked_mut(engine_index) =
                Some(instance_pre)
        };
    }
}

pub struct InstancePreCache {
    map: SynchronizedMap<SHA256Hash, InstancePreCacheEntry>,
    engine_count: usize,
}

impl InstancePreCache {
    pub fn new(engine_count: usize) -> Self {
        Self {
            map: SynchronizedMap::new(),
            engine_count,
        }
    }

    pub fn get(
        &self,
        hash: &SHA256Hash,
        engine_index: usize,
    ) -> Option<Arc<InstancePre<*mut c_void>>> {
        // Use find_read() for concurrent read access - multiple threads can
        // read the same cache entry simultaneously without blocking each other.
        self.map.find_read(hash)?.value().get(engine_index).clone()
    }

    fn get_or_create_entry(
        &self,
        hash: &SHA256Hash,
    ) -> synchronized_map::BucketGuard<'_, SHA256Hash, InstancePreCacheEntry>
    {
        let engine_count = self.engine_count;
        self.map.get_or_insert_with(*hash, || {
            InstancePreCacheEntry::new(engine_count)
        })
    }

    pub fn put(
        &self,
        hash: &SHA256Hash,
        engine_index: usize,
        instance_pre: Arc<InstancePre<*mut c_void>>,
    ) {
        let mut guard = self.get_or_create_entry(hash);
        guard.value().put(engine_index, instance_pre);
    }
}

// Compile-time assertion: InstancePreCache and the underlying
// SynchronizedMap with its real value type are Send + Sync. The cache
// lives behind an Arc<WasmtimeGlobalContext> shared across threads;
// concurrent access is the whole point. If wasmtime ever changes
// InstancePre or we add a !Send/!Sync field to InstancePreCacheEntry,
// this fails to compile.
const _: fn() = || {
    fn assert_send_sync<T: Send + Sync>() {}
    assert_send_sync::<InstancePreCache>();
    assert_send_sync::<SynchronizedMap<SHA256Hash, InstancePreCacheEntry>>();
};
