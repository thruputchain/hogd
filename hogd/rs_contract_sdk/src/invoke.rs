#![allow(non_snake_case)]

use crate::syscall_bindings;

use rs_xdr::ContractAddress;
use core::mem::MaybeUninit;

pub fn GetMsgSender() -> Result<ContractAddress, i32> {

    let mut out_addr: MaybeUninit<ContractAddress> = MaybeUninit::<ContractAddress>::uninit();

    match syscall_bindings::uninit_GetMsgSender(&mut out_addr) {
        Ok(()) => Ok( unsafe {out_addr.assume_init()}),
        Err(x) => Err(x)
    }
}
