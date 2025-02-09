#![cfg_attr(not(test), no_std)]

use groundhog_xdr;
use rs_contract_sdk as sdk;

#[cfg(test)]
use test_ffi;

#[cfg(test)]
mod tests {
    use super::*;

    use sha2::Digest;
    use sha2::Sha256;

    static mut x: bool = false;
    #[test]
    fn invoke_err() {
        let mut env = test_ffi::types::Environment::new();
        let mut ctx = test_ffi::types::MockContractContext::new();
        let mut contract = ctx.add_contract();

        env.advance_block();

        pub fn test_basic() -> i32 {
            unsafe { x = true };
            return 10;
        }

        contract.register_method(0, test_basic);

        let tx = test_ffi::deployer::make_deploy_and_invoke_transaction(
            &contract,
            &[0; 32],
            0,
            &[],
        );

        let mut tx_handler =
            test_ffi::types::TransactionHandler::new(&env, &ctx, &tx);

        let res = tx_handler.root_invoke();
        assert_eq!(
            res.result.panic,
            groundhog_xdr::InvokeErrorCode::kDeterministic as i32
        );
        assert!(unsafe { x });
    }

    #[test]
    fn invoke_ok() {
        let mut env = test_ffi::types::Environment::new();
        let mut ctx = test_ffi::types::MockContractContext::new();
        let mut contract = ctx.add_contract();

        env.advance_block();

        pub fn test_return_success() -> i32 {
            return 0;
        }

        contract.register_method(1, test_return_success);

        let tx = test_ffi::deployer::make_deploy_and_invoke_transaction(
            &contract,
            &[0; 32],
            1,
            &[],
        );

        let mut tx_handler =
            test_ffi::types::TransactionHandler::new(&env, &ctx, &tx);

        let res = tx_handler.root_invoke();
        assert_eq!(
            res.result.panic,
            groundhog_xdr::InvokeErrorCode::kNone as i32
        );
    }

    static mut hash_expect: [u8; 32] = [0; 32];
    #[test]
    fn check_sha256_syscall() {
        let mut hasher = Sha256::new();
        hasher.update([0, 1, 2, 3]);
        unsafe { hash_expect = hasher.finalize().try_into().unwrap() };

        let mut env = test_ffi::types::Environment::new();
        let mut ctx = test_ffi::types::MockContractContext::new();
        let mut contract = ctx.add_contract();

        env.advance_block();

        contract.register_method(2, || -> i32 {
            println!("whoa");
            let data: [u8; 4] = [0, 1, 2, 3];
            let res = ::rs_contract_sdk::SHA256(&data).unwrap();
            assert_eq!(res, unsafe { hash_expect });
            0
        });

        let tx = test_ffi::deployer::make_deploy_and_invoke_transaction(
            &contract,
            &[0; 32],
            2,
            &[],
        );

        let mut tx_handler =
            test_ffi::types::TransactionHandler::new(&env, &ctx, &tx);

        let res = tx_handler.root_invoke();
        assert_eq!(
            res.result.panic,
            groundhog_xdr::InvokeErrorCode::kNone as i32
        );
    }
}
