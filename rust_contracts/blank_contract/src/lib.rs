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
    use groundhog_xdr::types::StorageKey;
    use groundhog_xdr::{Limits, WriteXdr};

    static mut X: bool = false;
    #[test]
    fn invoke_err() {
        let mut env = test_ffi::TestEnvironment::new();

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

        let mut tx_handler = test_ffi::TransactionHandler::new(&env, &tx);

        let res = tx_handler.root_invoke();
        assert_eq!(res.result.panic, InvokeErrorCode::kDeterministic as i32);
        assert!(unsafe { X });
    }

    #[test]
    fn invoke_ok() {
        let mut env = test_ffi::TestEnvironment::new();

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

        let mut tx_handler = test_ffi::TransactionHandler::new(&env, &tx);

        let res = tx_handler.root_invoke();
        assert_eq!(res.result.panic, InvokeErrorCode::kNone as i32);
    }

    static mut HASH_EXPECT: [u8; 32] = [0; 32];
    #[test]
    fn check_sha256_syscall() {
        let mut hasher = Sha256::new();
        hasher.update([0, 1, 2, 3]);
        unsafe { HASH_EXPECT = hasher.finalize().try_into().unwrap() };

        let mut env = test_ffi::TestEnvironment::new();

        let code = {
            let mut contract = env.add_contract();

            contract.register_method(2, || -> i32 {
                println!("print from within a contract with --nocapture flag");
                let data: [u8; 4] = [0, 1, 2, 3];
                let res = ::rs_contract_sdk::crypto::SHA256(&data).unwrap();
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
            &[6, 0, 1, 2, 3, 4, 5],
        );

        let mut tx_handler = test_ffi::TransactionHandler::new(&env, &tx);

        let res = tx_handler.root_invoke();
        assert_eq!(res.result.panic, InvokeErrorCode::kNone as i32);
    }

    #[test]
    fn check_prepare_test() {
        let mut env = test_ffi::TestEnvironment::new();

        // first write an int, then read it back

        let contract = {
            let mut contract = env.add_contract();

            contract.register_method(0, || -> i32 {
                println!("first method");
                let key: StorageKey = [0; 32];
                ::rs_contract_sdk::NonnegativeIntSetAdd(&key, 100, 50).unwrap();
                0
            });

            contract.register_method(1, || -> i32 {
                println!("second method");
                let key: StorageKey = [0; 32];
                let (s, a) =
                    ::rs_contract_sdk::NonnegativeIntGet(&key).unwrap();
                assert_eq!(s, 100);
                assert_eq!(a, 50);
                0
            });

            contract
        };

        env.setup_create_contract(&contract.as_contract());

        let contract_bytes =
            contract.as_contract().to_xdr(Limits::none()).unwrap();

        let mut handler = env.prepare_test(move || -> i32 {
            println!("handler method");
            let contract_hash =
                ::rs_contract_sdk::crypto::SHA256(&contract_bytes).unwrap();
            let deployed_addr = ::rs_contract_sdk::deploy::DeployContract(
                &[0; 32],
                &contract_hash,
            )
            .unwrap();
            println!("here!");

            ::rs_contract_sdk::sys::raw_Invoke(
                &deployed_addr,
                0,
                u64::MAX,
                &[],
                0,
            )
            .unwrap();
            ::rs_contract_sdk::sys::raw_Invoke(
                &deployed_addr,
                1,
                u64::MAX,
                &[],
                0,
            )
            .unwrap();

            return 0;
        });

        let res = handler.root_invoke();
        assert_eq!(res.result.panic, InvokeErrorCode::kNone as i32);
    }
}
