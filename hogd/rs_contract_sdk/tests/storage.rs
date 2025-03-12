use rs_contract_sdk as sdk;

#[test]
fn test_storage() {
    let mut env = sdk::TestEnvironment::new();

    let test_contract = {
        let mut contract = env.add_contract();
        contract.register_method(1, method_one_internal);
        contract.as_contract()
    };

    let contract_hash = env.setup_create_contract(&test_contract);

    let mut handler = env.prepare_test(move || -> i32 {
        match test_storage_inner(&contract_hash) {
            Ok(_) => 0,
            Err(error) => panic!("test failed: {}", error),
        }
    });

    let res = handler.root_invoke();
    assert_eq!(res.result.panic, sdk::InvokeErrorCode::kNone as i32);
}

fn test_storage_inner(contract_hash: &sdk::types::SHA256Hash) -> sdk::result::Result<()> {
    use sdk::prelude::*;

    let nonce = ByteArray::new([7; 32]);
    let deployed_addr = DeployContract(&nonce, contract_hash)?;

    let _ = sdk::internal::invoke::<(),()>(&deployed_addr, 1, u64::MAX, &());

    Ok(())
}

fn method_one_internal() -> i32 {
    sdk::internal::handle_call(|()| {
        method_one()
    }).into()
}

fn method_one() {
    use sdk::prelude::*;

    Log(b"\xDE\xAD").unwrap();
}
