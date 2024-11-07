use core::mem::MaybeUninit;
use rs_contract_sdk as sdk;
use sdk::parse_trait::Serializable;

pub fn compute_balance_key(addr: &sdk::ContractAddress) -> sdk::SHA256Hash {
    sdk::SHA256(addr).unwrap()
}

fn compute_allowance_key(
    owner: &sdk::ContractAddress,
    spender: &sdk::ContractAddress,
) -> sdk::SHA256Hash {
    let mut bytes_mu = [MaybeUninit::<u8>::uninit(); 64];

    (owner, spender).do_serialize(&mut bytes_mu).unwrap();

    let bytes: [u8; 64] = unsafe { core::mem::transmute(bytes_mu) };

    sdk::SHA256(&bytes).unwrap()
}

pub fn do_transfer(
    from: &sdk::ContractAddress,
    to: &sdk::ContractAddress,
    amount: i64,
) {
    if amount < 0 {
        sdk::rt::trap();
    }

    let from_key = compute_balance_key(from);
    let to_key = compute_balance_key(to);

    sdk::NonnegativeInt64Add(&from_key, -amount).unwrap();
    sdk::NonnegativeInt64Add(&to_key, amount).unwrap();
}

pub fn do_mint(to: &sdk::ContractAddress, amount: i64) {
    if amount < 0 {
        sdk::rt::trap();
    }

    let to_key = compute_balance_key(to);
    sdk::NonnegativeInt64Add(&to_key, amount).unwrap();
}

pub fn do_burn(to: &sdk::ContractAddress, amount: i64) {
    if amount > 0 || amount == i64::MIN {
        // would overflow when we take negative
        sdk::rt::trap();
    }

    let to_key = compute_balance_key(to);
    sdk::NonnegativeInt64Add(&to_key, -amount).unwrap();
}

pub fn do_allowance_delta(
    owner: &sdk::ContractAddress,
    spender: &sdk::ContractAddress,
    amount: i64,
) {
    let balance_key = compute_allowance_key(owner, spender);
    sdk::NonnegativeInt64Add(&balance_key, amount).unwrap();
}
