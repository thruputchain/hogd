#![no_std]

extern crate alloc;

use alloc::vec::Vec;
use sdk::contract::Contract;
use sdk::deploy::DeployContract;
use sdk::log::Log;
use sdk::macros::contract;
use sdk::sys;
use sdk::types::{MethodID, SHA256Hash, StorageKey};

struct GenesisDeployer;

impl Contract for GenesisDeployer {
    fn new(_: &StorageKey) -> Self {
        Self
    }
}

#[contract(export)]
impl GenesisDeployer {
    pub fn deploy(self, nonce: &SHA256Hash, contract_hash: &SHA256Hash) {
        let _ = DeployContract(nonce, contract_hash).unwrap();
    }

    pub fn deploy_and_invoke(
        self,
        nonce: &SHA256Hash,
        contract_hash: &SHA256Hash,
        method: MethodID,
        calldata: Vec<u8>,
    ) {
        let deployed_addr = DeployContract(nonce, contract_hash).unwrap();

        sys::raw_Invoke(
            &deployed_addr,
            method,
            u64::MAX,
            calldata.as_slice(),
            0,
        )
        .unwrap();
        Log(b"ABCDEFG").unwrap();
    }
}
