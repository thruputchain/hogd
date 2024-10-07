use rs_xdr::{SyscallNo, SyscallErrorCode};
use crate::syscall::syscall;

use crate::types::Address;
use core::mem::MaybeUninit;

pub fn get_msg_sender() -> Result<Address, i32> {

    let mut out_addr: MaybeUninit<Address> = MaybeUninit::<Address>::uninit();//

    let err: i32 = unsafe {
        // ignore return on this syscall
        syscall(
            SyscallNo::GetMsgSender,
            out_addr.as_mut_ptr() as u64,
            32,
            0,
            0,
        )
    };

    if err != SyscallErrorCode::None as i32 {
        return Err(err);
    }

    unsafe { Ok(out_addr.assume_init()) }
}
