use wasmtime::{InstancePre, Engine};

use core::ffi::c_void;


pub type InstancePreGetFn = unsafe extern "C" fn(
    hash: *const u8, // 32 bytes
    external_cache: *mut c_void,
) -> *mut c_void;  // returns a pointer to the InstancePre object

pub type InstancePrePutFn = unsafe extern "C" fn(
    hash: *const u8, // 32 bytes
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
        Self { instance_pre_get_fn, instance_pre_put_fn, external_cache }
    }

    pub fn get(
        &self,
        hash: &[u8; 32], // 32 bytes
    ) -> Option<InstancePre<*mut c_void>> {
        let instance_pre = unsafe {
            (self.instance_pre_get_fn)(hash.as_ptr(), self.external_cache)
        };
        if instance_pre.is_null() {
            None
        } else {
            // The pointer points to a boxed InstancePre that we need to clone
            // We create a reference to it, clone the InstancePre, and leave the box intact
            let instance_pre_ptr: *mut InstancePre<*mut c_void> = instance_pre as *mut InstancePre<*mut c_void>;
            let instance_pre_ref = unsafe { &*instance_pre_ptr };
            Some(instance_pre_ref.clone())
        }
    }

    pub fn put(
        &self,
        hash: &[u8; 32], // 32 bytes
        instance_pre: &InstancePre<*mut c_void>,
    ) {
        // Clone the InstancePre and box it
        let cloned = instance_pre.clone();
        let instance_pre_box = Box::new(cloned);
        let instance_pre_ptr = Box::into_raw(instance_pre_box);
        let instance_pre_ptr_void = instance_pre_ptr as *mut c_void;
        unsafe {
            (self.instance_pre_put_fn)(hash.as_ptr(), instance_pre_ptr_void, self.external_cache)
        }
    }
}

#[no_mangle]
pub unsafe extern "C" fn free_instance_pre_from_cache(
    instance_pre: *mut c_void,
) {

    let inst: *mut InstancePre<*mut c_void> = unsafe { core::mem::transmute(instance_pre) };
    unsafe { drop(Box::from_raw(inst)); }
}