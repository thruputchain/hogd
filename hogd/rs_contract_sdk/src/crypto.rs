use rs_xdr::{SyscallNo, SyscallErrorCode};
use crate::syscall::syscall;

use crate::types::Hash;
use core::mem::MaybeUninit;

pub fn sha256(input : &[u8]) -> Result<Hash, i32> {

	let mut out_hash: MaybeUninit<Hash> = MaybeUninit::<Hash>::uninit();

    // syscall will initialize hash
	let err =
		syscall(SyscallNo::kSHA256,
			input.as_ptr() as u64,
			input.len() as u64,
			out_hash.as_mut_ptr() as u64,
			0);

	if err != SyscallErrorCode::kNone as i32 {
		return Err(err);
	}

	unsafe {
		Ok(out_hash.assume_init())
	}
}

