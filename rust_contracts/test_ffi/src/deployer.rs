use crate::types::MockContract;
use groundhog_xdr::transaction::{Invocation, SignedTransaction, Transaction};
use groundhog_xdr::types::SHA256Hash;

pub fn make_deploy_transaction(
    contract: &MockContract,
    nonce: &SHA256Hash,
) -> SignedTransaction {
    let concatenated: [u8; 64] = {
        let mut arr = [0u8; 64];
        arr[..32].copy_from_slice(nonce);
        arr[32..].copy_from_slice(&contract.as_hash());
        arr
    };

    SignedTransaction {
        tx: Transaction {
            invocation: Invocation {
                invoked_address: [0; 32],
                method: MockContract::method_int_from_sdk_name("deploy"),
                gas_limit: u64::MAX,
                calldata: concatenated.try_into().unwrap(),
            },
            contracts_to_deploy: vec![contract.as_contract()]
                .try_into()
                .unwrap(),
        },
    }
}

pub fn make_deploy_and_invoke_transaction(
    contract: &MockContract,
    nonce: &SHA256Hash,
    method: u32,
    calldata: &[u8],
) -> SignedTransaction {
    let mut c: Vec<u8> = vec![];
    c.extend(nonce);
    c.extend(contract.as_hash());
    c.extend(u32::to_le_bytes(method));
    c.extend(calldata);

    SignedTransaction {
        tx: Transaction {
            invocation: Invocation {
                invoked_address: [0; 32],
                method: MockContract::method_int_from_sdk_name(
                    "deploy_and_invoke",
                ),
                gas_limit: u64::MAX,
                calldata: c.try_into().unwrap(),
            },
            contracts_to_deploy: vec![contract.as_contract()]
                .try_into()
                .unwrap(),
        },
    }
}
