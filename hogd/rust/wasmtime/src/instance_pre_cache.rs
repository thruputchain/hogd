use wasmtime::InstancePre;

use core::ffi::c_void;
use std::sync::Arc;

// # Safety:
// This returns an Arc<InstancePre> reference that is owned by the caller.
pub type InstancePreGetFn = unsafe extern "C" fn(
    hash: *const [u8; 32],
    external_cache: *mut c_void,
) -> *mut c_void;

pub type InstancePrePutFn = unsafe extern "C" fn(
    hash: *const [u8; 32],
    instance_pre: *mut c_void,
    external_cache: *mut c_void,
);

#[derive(Clone)]
pub struct InstancePreCache {
    instance_pre_get_fn: InstancePreGetFn,
    instance_pre_put_fn: InstancePrePutFn,
    external_cache: *mut core::ffi::c_void,
}

impl InstancePreCache {
    pub fn new(
        instance_pre_get_fn: InstancePreGetFn,
        instance_pre_put_fn: InstancePrePutFn,
        external_cache: *mut c_void,
    ) -> Self {
        Self {
            instance_pre_get_fn,
            instance_pre_put_fn,
            external_cache,
        }
    }

    pub fn get(
        &self,
        hash: &[u8; 32], // 32 bytes
    ) -> Option<Arc<InstancePre<*mut c_void>>> {
        let instance_pre =
            unsafe { (self.instance_pre_get_fn)(hash, self.external_cache) };
        if instance_pre.is_null() {
            None
        } else {
            let instance_pre_arc = unsafe {
                Arc::from_raw(instance_pre as *mut InstancePre<*mut c_void>)
            };

            Some(instance_pre_arc)
        }
    }

    pub fn put(
        &self,
        hash: &[u8; 32],
        instance_pre: Arc<InstancePre<*mut c_void>>,
    ) {
        debug_assert!(Arc::strong_count(&instance_pre) == 2);
        let instance_pre_ptr = Arc::into_raw(instance_pre);
        let instance_pre_ptr_void = instance_pre_ptr as *mut c_void;
        // External cache holds a reference to the InstancePre
        // That refcount is released by the into_raw call
        unsafe {
            (self.instance_pre_put_fn)(
                hash,
                instance_pre_ptr_void,
                self.external_cache,
            )
        }
    }
}

#[no_mangle]
pub unsafe extern "C" fn arc_instance_pre_drop_ref(instance_pre: *mut c_void) {
    let inst: *mut InstancePre<*mut c_void> =
        unsafe { core::mem::transmute(instance_pre) };
    let _instance_pre_arc = unsafe { Arc::from_raw(inst) };
    // drop the ref
}

#[no_mangle]
pub unsafe extern "C" fn arc_instance_pre_clone_ref(instance_pre: *mut c_void) {
    let inst: *mut InstancePre<*mut c_void> =
        unsafe { core::mem::transmute(instance_pre) };
    unsafe { Arc::increment_strong_count(inst) };
}
