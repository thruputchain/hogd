use synchronized_map::SynchronizedMap;

use crate::SHA256Hash;
use wasmtime::InstancePre;

use core::ffi::c_void;

use std::sync::Arc;

pub struct InstancePreCache {
    map: SynchronizedMap<SHA256Hash, Arc<InstancePre<*mut c_void>>>,
}

impl InstancePreCache {
    pub fn new() -> Self {
        Self {
            map: SynchronizedMap::new(),
        }
    }

    pub fn get(
        &self,
        hash: &SHA256Hash,
    ) -> Option<Arc<InstancePre<*mut c_void>>> {
        if let Some(mut guard) = self.map.find(hash) {
            Some(guard.value().clone())
        } else {
            None
        }
    }

    pub fn put(
        &self,
        hash: &SHA256Hash,
        instance_pre: Arc<InstancePre<*mut c_void>>,
    ) {
        self.map.insert(*hash, instance_pre);
    }
}
