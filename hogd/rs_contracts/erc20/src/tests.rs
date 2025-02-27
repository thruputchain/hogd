use sdk::types::ByteArray;
use crate::implementation;
use crate::interface::{client, ERC20};

#[test]
fn check_mint() {
    let mut env = sdk::TestEnvironment::new();

    let test_contract = implementation::test_setup::register(&mut env);
    let contract_hash = env.setup_create_contract(&test_contract);

    let mut handler = env.prepare_test(move || -> i32 {
        let nonce = ByteArray::new([7; 32]);
        let deployed_addr =
            sdk::deploy::DeployContract(&nonce, &contract_hash).unwrap();

        let mut client = client::ERC20Client::new(&deployed_addr);

        let owner = sdk::invoke::GetCurrentAddress().unwrap();

        client.initialize(&owner).unwrap();

        client.mint(&owner, 100).unwrap();
        assert_eq!(client.get_balance().unwrap(), 100);

        let other = ByteArray::new([0xBB; 32]);
        client.transfer(&other, 25).unwrap();
        assert_eq!(client.get_balance_of(&other).unwrap(), 25);
        assert_eq!(client.get_balance_of(&owner).unwrap(), 75);

        client.burn(&other, 5).unwrap();
        assert_eq!(client.get_balance_of(&other).unwrap(), 20);

        assert_eq!(client.total_supply().unwrap(), 95);

        return 0;
    });

    let res = handler.root_invoke();
    assert_eq!(res.result.panic, sdk::InvokeErrorCode::kNone as i32);
}
