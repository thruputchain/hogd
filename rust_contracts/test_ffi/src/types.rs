use core::ffi::c_void;

include!(concat!(env!("OUT_DIR"), "/bindings.rs"));

pub struct DBWrapper {
    db: *mut c_void
}

impl DBWrapper {
    pub fn new() -> Self {
        Self {
            db: unsafe {create_db()}
        }
    }
}

impl Drop for DBWrapper {
    fn drop(&mut self) {
        unsafe {free_db(self.db)}
    }
}
