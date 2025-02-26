use crate::interface::ERC20;
use crate::*;
use sdk::types::ByteArray;

#[test]
fn check_mint() {
    let mut env = sdk::TestEnvironment::new();

    let test_contract = guest::test_setup::register(&mut env);
    let contract_hash = env.setup_create_contract(&test_contract);

    let mut handler = env.prepare_test(move || -> i32 {
        let nonce = ByteArray::new([0; 32]);
        let deployed_addr =
            sdk::deploy::DeployContract(&nonce, &contract_hash).unwrap();

        let mut client = client::ERC20Client::new(&deployed_addr);

        let self_addr = sdk::invoke::GetCurrentAddress().unwrap();

        client.initialize(&self_addr).unwrap();

        client.mint(&self_addr, 100).unwrap();

        assert_eq!(client.get_balance().unwrap(), 100);

        return 0;
    });

    let res = handler.root_invoke();
    assert_eq!(res.result.panic, sdk::InvokeErrorCode::kNone as i32);
}
