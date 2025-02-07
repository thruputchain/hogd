use core::ffi::c_void;

include!(concat!(env!("OUT_DIR"), "/bindings.rs"));

use groundhog_xdr::{SHA256Hash, InvokeErrorCode};
use groundhog_xdr::{Limits, WriteXdr};

use sha2::Digest;
use sha2::Sha256;

#[cfg(test)]
use std::panic::panic_any;
use std::panic;
use std::error::Error;
use std::fmt;
use std::any::Any;

/*
struct ForeignInvokeResult {
  int32_t result_code;
  int32_t panic;
};
*/
#[derive(Debug, PartialEq)]
#[repr(C)]
pub struct InvokeResult {
    result_code: i32,
    panic: i32,
}

use rs_contract_sdk::InvokeError;

pub fn invoke_wrapper(wrapped: fn() -> i32) -> Box<dyn Fn() -> InvokeResult> {
    Box::new(move || {
        // This wrapper never returns InvokeErrorCode::kOutOfGas.
        let result = panic::catch_unwind(|| wrapped());

        match result {
            Ok(result_code) => {
                return InvokeResult {
                    result_code: result_code,
                    panic: InvokeErrorCode::kNone as i32,
                }
            }
            Err(err) => {
                if let Some(my_error) = err.downcast_ref::<InvokeError>() {
                    return InvokeResult {
                        result_code: 0,
                        panic: my_error.error.clone() as i32,
                    };
                }
                return InvokeResult {
                    result_code: 0,
                    panic: InvokeErrorCode::kUnrecoverable as i32,
                };
            }
        }
    })
}

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
    fn new(n: u32) -> Self {
        let mut hasher = Sha256::new();
        hasher.update(n.to_xdr(Limits::none()).unwrap());
        let result = hasher.finalize();
        Self {
            index: n,
            hash: result.into(),
        }
    }

    pub fn as_contract(&self) -> groundhog_xdr::Contract {
        let bytes = self.index.to_xdr(Limits::none()).unwrap();
        groundhog_xdr::Contract {
            contract_data: bytes.try_into().unwrap(),
        }
    }
}

pub struct MockContractContext {
    context: *mut c_void,
    mock_contract_index: u32,
    funcs: Vec<*mut dyn Fn() -> InvokeResult>
}

impl MockContractContext {
    pub fn new() -> Self {
        Self {
            context: unsafe { open_runtime_context() },
            mock_contract_index: 0,
            funcs: vec![]
        }
    }

    pub fn add_contract(&mut self) -> MockContract {
        let out = MockContract::new(self.mock_contract_index);
        self.mock_contract_index += 1;
        out
    }

    pub fn method_name(method_id: u32) -> String {
        format!("{:X}", method_id)
    }

    pub fn add_method(
        &mut self,
        contract: &MockContract,
        name: &[u8],
        pointer: fn() -> i32,
    ) {
        let new_func = invoke_wrapper(pointer);
        let out = Box::<dyn Fn() -> InvokeResult>::into_raw(new_func);
        self.funcs.push(out);

        unsafe {
            add_contract_and_method(
                self.context,
                contract.hash.as_ptr(),
                name.as_ptr(),
                name.len().try_into().unwrap(),
                out as *mut c_void
            )
        }
    }
}

impl Drop for MockContractContext {
    fn drop(&mut self) {
        unsafe { close_runtime_context(self.context) }
        for func in &self.funcs {
            unsafe {
                let _ : Box<dyn Fn() -> InvokeResult> = Box::from_raw(*func);
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    // There's a race condition on the panic handler,
    // which changes how tests behave
    #[test]
    fn test_panic_runner() {
        test_error_wrap();
        test_panic();
        test_panic_ok();
        test_sdk_panic();
        test_option_panic();
        test_pass_invoke_error_through_panic_hook();
    }


    fn test_error_wrap() {
        panic::take_hook();
        let f: fn() -> i32 = || {
            return 1;
        };

        let boxed = invoke_wrapper(f);

        assert_eq!((*boxed)(), InvokeResult {
            result_code : 1,
            panic: 0
        });
    }

    fn test_panic() {
        panic::take_hook();
        let f: fn() -> i32 = || {
            panic!("unknown")
        };

        let boxed = invoke_wrapper(f);

        assert_eq!((*boxed)(), InvokeResult {
            result_code : 0,
            panic: InvokeErrorCode::kUnrecoverable as i32
        });
    }

    fn test_panic_ok() {
        panic::take_hook();
        let f: fn() -> i32 = || {
            panic_any(
                InvokeError {
                    error: InvokeErrorCode::kValidationFailure
                }
            )
        };

        let boxed = invoke_wrapper(f);

        assert_eq!((*boxed)(), InvokeResult {
            result_code : 0,
            panic: InvokeErrorCode::kValidationFailure as i32
        });
    }

    fn test_sdk_panic() {
        rs_contract_sdk::set_override_panic_hook();
        let f: fn() -> i32 = || {
            panic!("something");
        };

        let boxed = invoke_wrapper(f);

        assert_eq!((*boxed)(), InvokeResult {
            result_code : 0,
            panic: InvokeErrorCode::kDeterministic as i32
        });
    }

    fn test_option_panic() {
        rs_contract_sdk::set_override_panic_hook();
        let f: fn() -> i32 = || {
            let x : Option<i32> = None;
            return x.unwrap();
        };

        let boxed = invoke_wrapper(f);

        assert_eq!((*boxed)(), InvokeResult {
            result_code : 0,
            panic: InvokeErrorCode::kDeterministic as i32
        });
    }

    fn test_pass_invoke_error_through_panic_hook() {
        rs_contract_sdk::set_override_panic_hook();
        let f: fn() -> i32 = || {
            panic_any(
                InvokeError {
                    error : InvokeErrorCode::kValidationFailure
                }
            )
        };

        let boxed = invoke_wrapper(f);

        assert_eq!((*boxed)(), InvokeResult {
            result_code : 0,
            panic: InvokeErrorCode::kValidationFailure as i32
        });
    }
}
