use groundhog_xdr::syscall::{SyscallErrorCode, SyscallNo};
use groundhog_xdr::types::SHA256Hash;

#[cfg(feature="mock")]
use groundhog_xdr::{ReadXdr, WriteXdr};
#[cfg(feature="mock")]
use groundhog_xdr::{SHA256_input, SHA256_output};
#[cfg(feature="mock")]
use test_ffi::invoke_error::unrecoverable_error;
#[cfg(feature="mock")]
use std::panic::panic_any;

use core::mem::MaybeUninit;

#[cfg(not(feature="mock"))]
pub fn uninit_SHA256(
    buffer: &[u8],
    hash: &mut MaybeUninit<SHA256Hash>,
) -> Result<(), i32> {
    // syscall will initialize outputs, if error is kNone
    panic!("unimpl");
    /*let err: SysErr = syscall_wrapper(
        SyscallNo::kSHA256,
        buffer.as_ptr() as u64,
        buffer.len() as u64,
        hash.as_mut_ptr() as u64,
        0,
        0,
        0,
    ); */

   /*if !err.is_none() {
        return Err(err);
    }*/

    Ok(())
}

#[cfg(feature="mock")]
pub fn uninit_SHA256(
    buffer: &[u8],
    hash: &mut MaybeUninit<SHA256Hash>
) -> Result<(), i32> {

    let input = SHA256_input {
        buffer: buffer
            .try_into()
            .map_err(|_| panic_any(unrecoverable_error()))
            .unwrap(),
    };

    // allocate space for syscall to write results
    let mut output = SHA256_output { hash: [0; 32] };
    let mut out_buffer = output
        .to_xdr(groundhog_xdr::Limits::none())
        .map_err(|_| panic_any(unrecoverable_error()))
        .unwrap();
    let input_slice = input
        .to_xdr(groundhog_xdr::Limits::none())
        .map_err(|_| panic_any(unrecoverable_error()))
        .unwrap();

    let out_code = test_ffi::perform_mocked_syscall(
        SyscallNo::kSHA256,
        &input_slice,
        &mut out_buffer,
    );

    if out_code != SyscallErrorCode::kNone as i32 {
        return Err(out_code);
    }

    output =
        SHA256_output::from_xdr(&out_buffer, groundhog_xdr::Limits::none())
            .map_err(|_| panic_any(unrecoverable_error()))
            .unwrap();

    hash.write(output.hash);
    Ok(())
}

pub fn raw_SHA256(buffer: &[u8], hash: &mut SHA256Hash) -> Result<(), i32> {
    uninit_SHA256(buffer, unsafe { core::mem::transmute(hash) })
}

pub fn SHA256(bytes: &[u8]) -> Result<SHA256Hash, i32> {
    let mut out_hash = core::mem::MaybeUninit::<SHA256Hash>::uninit();
    uninit_SHA256(&bytes, &mut out_hash)?;
    let hash = unsafe { out_hash.assume_init() };
    Ok(hash)
}

/*
#[cfg(all(target_arch = "wasm32", not(feature = "mock")))]
mod wasm {
    use groundhog_xdr::SHA256Hash;

    pub fn SHA256(bytes: &[u8]) -> Result<SHA256Hash, i32> {
        panic!("unimplemented");
    }
}

#[cfg(feature = "mock")]
mod mock {
    use groundhog_xdr::syscall::{SyscallErrorCode, SyscallNo};
    use groundhog_xdr::types::SHA256Hash;
    use groundhog_xdr::{ReadXdr, WriteXdr};
    use groundhog_xdr::{SHA256_input, SHA256_output};

    use std::panic::panic_any;
    use test_ffi::invoke_error::unrecoverable_error;

    pub fn SHA256(bytes: &[u8]) -> Result<SHA256Hash, i32> {
        // This code has extra copies and serialization,
        // but this is not performance-critical nor ever deployed in prod.
        let input = SHA256_input {
            buffer: bytes
                .try_into()
                .map_err(|_| panic_any(unrecoverable_error()))
                .unwrap(),
        };

        // allocate space for syscall to write results
        let mut output = SHA256_output { hash: [0; 32] };
        let mut out_buffer = output
            .to_xdr(groundhog_xdr::Limits::none())
            .map_err(|_| panic_any(unrecoverable_error()))
            .unwrap();
        let input_slice = input
            .to_xdr(groundhog_xdr::Limits::none())
            .map_err(|_| panic_any(unrecoverable_error()))
            .unwrap();

        let out_code = test_ffi::perform_mocked_syscall(
            SyscallNo::kSHA256,
            &input_slice,
            &mut out_buffer,
        );

        if out_code != SyscallErrorCode::kNone as i32 {
            return Err(out_code);
        }

        output =
            SHA256_output::from_xdr(&out_buffer, groundhog_xdr::Limits::none())
                .map_err(|_| panic_any(unrecoverable_error()))
                .unwrap();

        Ok(output.hash)
    }
}

#[cfg(feature = "mock")]
pub use mock::*;
#[cfg(all(target_arch = "wasm32", not(feature = "mock")))]
pub use wasm::*;
*/
