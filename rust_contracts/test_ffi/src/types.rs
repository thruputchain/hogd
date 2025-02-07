use core::ffi::c_void;

include!(concat!(env!("OUT_DIR"), "/bindings.rs"));

use groundhog_xdr::SHA256Hash;
use groundhog_xdr::{Limits, WriteXdr};

use sha2::Digest;
use sha2::Sha256;

pub struct Environment {
    db: *mut c_void,
}

impl Environment {
    pub fn new() -> Self {
        Self {
            db: unsafe { create_db() },
        }
    }
}

impl Drop for Environment {
    fn drop(&mut self) {
        unsafe { free_db(self.db) };
    }
}

pub struct MockContract {
    index: u32, // this is passed as the raw bytes in the contract, and thus identified as sha256(index).
    hash: SHA256Hash,
}

impl MockContract {
    pub fn new(n: u32) -> Self {
        let mut hasher = Sha256::new();
        hasher.update(n.to_xdr(Limits::none()).unwrap());
        let result = hasher.finalize();
        Self {
            index: n,
            hash: result.into(),
        }
    }
}

pub struct MockContractContext {
    context: *mut c_void,
    mock_contract_index: u32,
}

impl MockContractContext {
    pub fn new() -> Self {
        Self {
            context: unsafe { open_runtime_context() },
            mock_contract_index: 0,
        }
    }

    pub fn add_method(
        &mut self,
        contract: &MockContract,
        name: &[u8],
        pointer: fn() -> i32,
    ) {
        unsafe {
            add_contract_and_method(
                self.context,
                contract.hash.as_ptr(),
                name.as_ptr(),
                name.len().try_into().unwrap(),
                pointer as *mut c_void,
            )
        }
    }
}

impl Drop for MockContractContext {
    fn drop(&mut self) {
        unsafe { close_runtime_context(self.context) }
    }
}
