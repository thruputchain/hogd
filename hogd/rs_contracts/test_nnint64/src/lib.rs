#![no_std]

use rs_contract_sdk as sdk;
use sdk::rt::trap;

fn make_set_add(
    key: sdk::StorageKey,
    set_value: i64,
    add_value: i64,
) -> Result<(), i32> {
    sdk::NonnegativeInt64SetAdd(&key, set_value, add_value)?;
    Ok(())
}

#[cfg_attr(target_family = "wasm", export_name = "0")]
#[no_mangle]
pub extern "C" fn do_set_add() -> i32 {
    let calldata = sdk::GetCalldata().unwrap();

    match sdk::parse_trait::parse_from_calldata_3::<sdk::StorageKey, i64, i64, ()>(
        &calldata,
        make_set_add,
    ) {
        Ok(()) => 0,
        Err(x) => x,
    }

    /*let key: sdk::StorageKey = calldata[0..32].try_into().unwrap();

        let set_value: i64 =
            i64::from_le_bytes(calldata[32..40].try_into().unwrap());
        let add_value: i64 =
            i64::from_le_bytes(calldata[40..48].try_into().unwrap());

        sdk::NonnegativeInt64SetAdd(&key, set_value, add_value).unwrap();
    */
}

#[cfg_attr(target_family = "wasm", export_name = "1")]
#[no_mangle]
pub extern "C" fn log_nnint64() -> i32 {
    let calldata = sdk::GetCalldata().unwrap();

    let key: sdk::StorageKey = calldata[0..32].try_into().unwrap();

    if calldata.len() != 32 {
        trap();
    }

    let val = sdk::NonnegativeInt64Get(&key).unwrap();

    sdk::Log(&val.to_le_bytes()).unwrap();

    0
}
