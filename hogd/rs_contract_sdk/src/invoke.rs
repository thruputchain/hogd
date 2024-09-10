use crate::syscall::{SyscallNo, SyscallErrorCode, syscall};

use crate::types::Address;
use core::mem::MaybeUninit;

pub fn get_msg_sender() -> Result<Address, u64> {

    let mut out_addr: MaybeUninit<Address> = MaybeUninit::<Address>::uninit();//

    let err: u64 = unsafe {
        // ignore return on this syscall
        syscall(
            SyscallNo::GetMsgSender as u64,
            out_addr.as_mut_ptr() as u64,
            32,
            0,
            0,
        )
    };

    if err != SyscallErrorCode::None as u64 {
        return Err(err);
    }

    unsafe { Ok(out_addr.assume_init()) }
}
