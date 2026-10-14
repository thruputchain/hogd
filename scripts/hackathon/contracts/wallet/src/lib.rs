#![no_std]
use groundhog_erc20_if::ERC20Client;
use sdk::transaction::InvocationFrameStatusCode;
use sdk::*;
const VARIANT: u64 = 1;

#[contract(client = Client)]
pub trait HackathonWallet {
    fn initialize(
        first: &Ed25519PublicKey,
        second: &Ed25519PublicKey,
        weight: u32,
    );
    fn call(target: &ContractAddress, method: u64, data: &[u8]);
    fn deploy(
        nonce: &SHA256Hash,
        hash: &SHA256Hash,
        initializer: u64,
        amount: i128,
    );
    fn refresh_auth(first: &Ed25519PublicKey, weight: u32);
}
pub struct Wallet;
fn authorized() {
    match sdk::get_auth_claim().unwrap() {
        AuthClaim::kSourceAddressAuthData {
            source_address_claim,
        } => {
            assert!(source_address_claim.required_weight > 0);
        }
        _ => trap(),
    }
}
fn invoke(target: &ContractAddress, method: u64, data: &[u8]) {
    let mut output = sdk::sys::CopyableInvokeOutputs {
        return_buffer_len: 0,
        returned_auth_token_count: 0,
        frame_status: InvocationFrameStatusCode::Fault,
    };
    sdk::sys::raw_Invoke(
        target,
        data,
        &[],
        &sdk::sys::CopyableInvokeInputs {
            entrypoint_id: method,
            gas_limit: u64::MAX,
            flags: 0,
            reserved: 0,
        },
        &mut output,
    )
    .unwrap();
    assert!(output.frame_status == InvocationFrameStatusCode::Return);
}
#[contract]
impl HackathonWallet for Wallet {
    fn initialize(
        first: &Ed25519PublicKey,
        second: &Ed25519PublicKey,
        weight: u32,
    ) {
        sdk::add_address_public_key(
            0,
            PublicKey::Ed25519 { ed25519: *first },
            1,
        )
        .unwrap();
        sdk::add_address_public_key(
            1,
            PublicKey::Ed25519 { ed25519: *second },
            1,
        )
        .unwrap();
        sdk::set_address_required_weight(weight).unwrap();
        non_negative_int_inc(&b"variant".hash().into(), VARIANT as i128);
    }
    fn call(target: &ContractAddress, method: u64, data: &[u8]) {
        authorized();
        invoke(target, method, data);
    }
    fn deploy(
        nonce: &SHA256Hash,
        hash: &SHA256Hash,
        initializer: u64,
        amount: i128,
    ) {
        authorized();
        let address = sdk::get_deploy_address(nonce, hash);
        ERC20Client::new(&sdk::precompiles::ADDRESS_PRECOMPILED_HOG_TOKEN)
            .transfer(&address, amount);
        sdk::deploy_contract(nonce, hash);
        invoke(&address, initializer, &[]);
    }
    fn refresh_auth(first: &Ed25519PublicKey, weight: u32) {
        authorized();
        // Vary the stored weight while keeping both the one-key and two-key
        // claims valid. This dirties auth data without invalidating the corpus.
        sdk::add_address_public_key(
            0,
            PublicKey::Ed25519 { ed25519: *first },
            weight,
        )
        .unwrap();
    }
}
