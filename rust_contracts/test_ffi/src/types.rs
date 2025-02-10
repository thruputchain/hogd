use core::ffi::c_void;

include!(concat!(env!("OUT_DIR"), "/bindings.rs"));

use groundhog_xdr::{InvokeErrorCode, SHA256Hash};
use groundhog_xdr::{Limits, WriteXdr};

use std::marker::PhantomPinned;
use std::pin::Pin;

use sha2::Digest;
use sha2::Sha256;

use std::collections::HashMap;

use std::panic;
#[cfg(test)]
use std::panic::panic_any;

type InvokeResult = ForeignInvokeResult;

impl PartialEq for ForeignInvokeResult {
    fn eq(&self, other: &Self) -> bool {
        (self.result_code == other.result_code) && (self.panic == other.panic)
    }
}

use crate::invoke_error::set_override_panic_hook;
use crate::invoke_error::InvokeError;

pub struct MockContract {
    index: u32, // this is passed as the raw bytes in the contract, and thus identified as sha256(index).
    hash: SHA256Hash,
    funcs: HashMap<u32, Box<dyn Fn() -> InvokeResult>>,
    _pin: PhantomPinned,
}

pub unsafe extern "C" fn trampoline(
    context: *mut c_void,
    method: u32,
) -> InvokeResult {
    let c: *mut MockContract = unsafe { core::mem::transmute(context) };

    match (*c).funcs.get(&method) {
        Some(x) => {
            return x();
        }
        None => {
            return InvokeResult {
                result_code: 0,
                panic: InvokeErrorCode::kDeterministic as i32,
            }
        }
    }
}

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
                        result_code: my_error.error_code,
                        panic: my_error.error_type.clone() as i32,
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
        set_override_panic_hook();
        Self {
            db: unsafe { create_db() },
        }
    }

    pub fn advance_block(&mut self) {
        unsafe { advance_new_block(self.db) }
    }
}

impl Drop for Environment {
    fn drop(&mut self) {
        unsafe { free_db(self.db) };
    }
}

impl MockContract {
    fn new(n: u32) -> Self {
        let mut hasher = Sha256::new();
        let contract = Self::u32_to_contract(n);
        hasher.update(contract.to_xdr(Limits::none()).unwrap());
        let result = hasher.finalize();
        Self {
            index: n,
            hash: result.into(),
            funcs: HashMap::new(),
            _pin: PhantomPinned,
        }
    }

    fn u32_to_contract(index: u32) -> groundhog_xdr::Contract {
        let bytes = index.to_xdr(Limits::none()).unwrap();
        groundhog_xdr::Contract {
            contract_data: bytes.try_into().unwrap(),
        }
    }

    pub fn as_contract(&self) -> groundhog_xdr::Contract {
        Self::u32_to_contract(self.index)
    }
    pub fn as_hash(&self) -> groundhog_xdr::SHA256Hash {
        self.hash
    }

    pub fn method_name_from_int(method_id: u32) -> String {
        format!("{:X}", method_id)
    }

    pub fn method_int_from_sdk_name(name: &str) -> u32 {
        let mut hasher = Sha256::new();
        hasher.update(name.as_bytes());
        let result = hasher.finalize();

        let prefix = &result[0..4];
        let num: u32 = u32::from_be_bytes(prefix.try_into().unwrap());
        num
    }

    pub fn register_method(
        self: &mut Pin<Box<Self>>,
        id: u32,
        func: fn() -> i32,
    ) {
        let unwrapped = unsafe { self.as_mut().get_unchecked_mut() }; // Safe only if the type itself upholds pin guarantees
        unwrapped.funcs.insert(id, invoke_wrapper(func));
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

    pub fn add_contract(&mut self) -> Pin<Box<MockContract>> {
        let mut out = Box::pin(MockContract::new(self.mock_contract_index));
        self.mock_contract_index += 1;

        let c: *mut MockContract =
            unsafe { core::mem::transmute(out.as_mut()) };

        let res = unsafe {
            add_contract_and_method(
                self.context,
                out.as_ref().as_hash().as_ptr(),
                Some(trampoline),
                c as *mut c_void,
            )
        };
        if res != 0 {
            panic!("failed to link fn");
        }
        out
    }
}

impl Drop for MockContractContext {
    fn drop(&mut self) {
        unsafe { close_runtime_context(self.context) }
    }
}

// Can run one, and only one, transaction,
// and then get the results
pub struct TransactionHandler<'a> {
    handler: *mut c_void,
    env: &'a Environment, // cannot outlive the environment.
    // TODO(geoff): we could swap the backing pointer to the db
    // to be a shared ptr, and have handler hold a reference,
    // like we do with the mock contract context.
    ctx: &'a MockContractContext,
    invoked: bool,
}

impl<'a> TransactionHandler<'a> {
    pub fn new(
        env: &'a Environment,
        ctx: &'a MockContractContext,
        stx: &groundhog_xdr::SignedTransaction,
    ) -> Self {
        let tx_bytes = stx.to_xdr(Limits::none()).unwrap();
        unsafe {
            Self {
                handler: open_syscall_handler(
                    env.db,
                    ctx.context,
                    tx_bytes.as_ptr(),
                    tx_bytes.len().try_into().unwrap(),
                ),
                env: env,
                ctx: ctx,
                invoked: false,
            }
        }
    }

    pub fn root_invoke(&mut self) -> ForeignRootInvokeResult {
        if self.invoked {
            panic!("should not double invoke");
        }
        self.invoked = true;

        unsafe { root_invoke(self.handler) }
    }
}

impl Drop for TransactionHandler<'_> {
    fn drop(&mut self) {
        unsafe { close_syscall_handler(self.handler) }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    // There's a race condition on the panic handler,
    // which changes how tests behave.  So run them all sequentially.
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
        let _ = panic::take_hook();
        let f: fn() -> i32 = || {
            return 1;
        };

        let boxed = invoke_wrapper(f);

        assert_eq!(
            (*boxed)(),
            InvokeResult {
                result_code: 1,
                panic: 0
            }
        );
    }

    fn test_panic() {
        let _ = panic::take_hook();
        let f: fn() -> i32 = || panic!("unknown");

        let boxed = invoke_wrapper(f);

        assert_eq!(
            (*boxed)(),
            InvokeResult {
                result_code: 0,
                panic: InvokeErrorCode::kUnrecoverable as i32
            }
        );
    }

    fn test_panic_ok() {
        let _ = panic::take_hook();
        let f: fn() -> i32 =
            || panic_any(crate::invoke_error::validation_error());

        let boxed = invoke_wrapper(f);

        assert_eq!(
            (*boxed)(),
            InvokeResult {
                result_code: 0,
                panic: InvokeErrorCode::kValidationFailure as i32
            }
        );
    }

    fn test_sdk_panic() {
        set_override_panic_hook();
        let f: fn() -> i32 = || {
            panic!("something");
        };

        let boxed = invoke_wrapper(f);

        assert_eq!(
            (*boxed)(),
            InvokeResult {
                result_code: 0,
                panic: InvokeErrorCode::kDeterministic as i32
            }
        );
    }

    fn test_option_panic() {
        set_override_panic_hook();
        let f: fn() -> i32 = || {
            let x: Option<i32> = None;
            return x.unwrap();
        };

        let boxed = invoke_wrapper(f);

        assert_eq!(
            (*boxed)(),
            InvokeResult {
                result_code: 0,
                panic: InvokeErrorCode::kDeterministic as i32
            }
        );
    }

    fn test_pass_invoke_error_through_panic_hook() {
        set_override_panic_hook();
        let f: fn() -> i32 =
            || panic_any(crate::invoke_error::validation_error());

        let boxed = invoke_wrapper(f);

        assert_eq!(
            (*boxed)(),
            InvokeResult {
                result_code: 0,
                panic: InvokeErrorCode::kValidationFailure as i32
            }
        );
    }
}
