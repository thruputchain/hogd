#![cfg_attr(not(test), no_std)]
#![allow(unused_imports)]
use rs_contract_sdk;

#[cfg(test)]
use test_ffi;

#[cfg(test)]
mod tests {
    use super::*;

    use sha2::Digest;
    use sha2::Sha256;

    use groundhog_xdr::ffi_enums::InvokeErrorCode;

    static mut X: bool = false;
    #[test]
    fn invoke_err() {
        let mut env = test_ffi::types::TestEnvironment::new();

        let code = {
            let mut contract = env.add_contract();

            contract.register_method(0, || -> i32 {
                unsafe { X = true };
                return 10;
            });

            contract.as_contract()
        };

        env.advance_block();

        let tx = test_ffi::deployer::make_deploy_and_invoke_transaction(
            code,
            &[0; 32],
            0,
            &[],
        );

        let mut tx_handler =
            test_ffi::types::TransactionHandler::new(&env, &tx);

        let res = tx_handler.root_invoke();
        assert_eq!(res.result.panic, InvokeErrorCode::kDeterministic as i32);
        assert!(unsafe { X });
    }

    #[test]
    fn invoke_ok() {
        let mut env = test_ffi::types::TestEnvironment::new();

        let code = {
            let mut contract = env.add_contract();

            contract.register_method(1, || -> i32 {
                return 0;
            });

            contract.as_contract()
        };

        env.advance_block();

        let tx = test_ffi::deployer::make_deploy_and_invoke_transaction(
            code,
            &[0; 32],
            1,
            &[],
        );

        let mut tx_handler =
            test_ffi::types::TransactionHandler::new(&env, &tx);

        let res = tx_handler.root_invoke();
        assert_eq!(res.result.panic, InvokeErrorCode::kNone as i32);
    }

    static mut HASH_EXPECT: [u8; 32] = [0; 32];
    #[test]
    fn check_sha256_syscall() {
        let mut hasher = Sha256::new();
        hasher.update([0, 1, 2, 3]);
        unsafe { HASH_EXPECT = hasher.finalize().try_into().unwrap() };

        let mut env = test_ffi::types::TestEnvironment::new();

        let code = {
            let mut contract = env.add_contract();

            contract.register_method(2, || -> i32 {
                println!("print from within a contract with --nocapture flag");
                let data: [u8; 4] = [0, 1, 2, 3];
                let res = ::rs_contract_sdk::SHA256(&data).unwrap();
                assert_eq!(res, unsafe { HASH_EXPECT });
                0
            });

            contract.as_contract()
        };

        env.advance_block();

        let tx = test_ffi::deployer::make_deploy_and_invoke_transaction(
            code,
            &[0; 32],
            2,
            &[0, 1, 2, 3, 4, 5],
        );

        let mut tx_handler =
            test_ffi::types::TransactionHandler::new(&env, &tx);

        let res = tx_handler.root_invoke();
        assert_eq!(res.result.panic, InvokeErrorCode::kNone as i32);
    }
}
