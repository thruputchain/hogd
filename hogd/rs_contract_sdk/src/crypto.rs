use crate::syscall::{SyscallNo, SyscallErrorCode, syscall};

use crate::types::Hash;
use core::mem::MaybeUninit;

pub fn sha256(input : &[u8]) -> Result<Hash, u64> {

	let mut out_hash: MaybeUninit<Hash> = MaybeUninit::<Hash>::uninit();

    // syscall will initialize hash
	let err = unsafe {
		syscall(SyscallNo::SHA256 as u64,
			input.as_ptr() as u64,
			input.len() as u64,
			out_hash.as_mut_ptr() as u64,
			0)
	};

	if err != SyscallErrorCode::None as u64 {
		return Err(err);
	}

	unsafe {
		Ok(out_hash.assume_init())
	}
}

