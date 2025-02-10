#[cfg(all(target_arch = "wasm32", not(feature = "mock")))]
mod wasm {
    use groundhog_xdr::SHA256Hash;

    pub fn SHA256(bytes: &[u8]) -> Result<SHA256Hash, i32> {
        panic!("unimplemented");
    }
}

#[cfg(feature = "mock")]
mod mock {
    use groundhog_xdr::SHA256Hash;
    use groundhog_xdr::{ReadXdr, WriteXdr};
    use groundhog_xdr::{SHA256_input, SHA256_output};
    use groundhog_xdr::{SyscallErrorCode, SyscallNo};

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
