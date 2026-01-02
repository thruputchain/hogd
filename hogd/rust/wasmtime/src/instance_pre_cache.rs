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
    ) -> Option<Arc<InstancePre<*mut c_void>>> {
        if engine_index >= self.instance_pres.len() {
            return None;
        }
        self.instance_pres[engine_index].clone()
    }

    pub fn put(
        &mut self,
        engine_index: usize,
        instance_pre: Arc<InstancePre<*mut c_void>>,
    ) {
        if engine_index < self.instance_pres.len() {
            self.instance_pres[engine_index] = Some(instance_pre);
        }
    }

    pub fn is_complete(&self) -> bool {
        self.instance_pres.iter().all(|x| x.is_some())
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
        if let Some(mut guard) = self.map.find(hash) {
            guard.value().get(engine_index)
        } else {
            None
        }
    }

    pub fn get_or_create_entry(
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
