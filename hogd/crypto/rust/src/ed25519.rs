#![no_std]

use ed25519_dalek::{
    verify_batch, Signature, Signer, SigningKey, Verifier, VerifyingKey,
};

/// Result codes for ed25519 operations
#[repr(C)]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Ed25519Result {
    Success = 0,
    InvalidPublicKey = 1,
    InvalidSignature = 2,
    VerificationFailed = 3,
    InvalidInputLength = 4,
}

/// Verify an ed25519 signature
///
/// # Parameters
/// - `public_key`: 32-byte public key array
/// - `signature`: 64-byte signature array
/// - `message_ptr`: Pointer to message bytes
/// - `message_len`: Length of message in bytes
///
/// # Returns
/// Ed25519Result indicating success or failure reason
///
/// # Safety
/// Caller must ensure:
/// - message_ptr points to valid message_len bytes
/// - message_ptr is valid for the duration of the call
/// - public_key points to valid 32 bytes
/// - signature points to valid 64 bytes
#[no_mangle]
pub unsafe extern "C" fn ed25519_dalek_verify(
    public_key: *const [u8; 32],
    signature: *const [u8; 64],
    message_ptr: *const u8,
    message_len: usize,
) -> Ed25519Result {
    debug_assert!(!public_key.is_null());
    debug_assert!(!signature.is_null());
    debug_assert!(!message_ptr.is_null());

    let public_key_bytes = unsafe { &*public_key };
    let signature_bytes = unsafe { &*signature };
    let message_bytes =
        unsafe { core::slice::from_raw_parts(message_ptr, message_len) };

    // Parse public key
    let verifying_key = match VerifyingKey::from_bytes(public_key_bytes) {
        Ok(key) => key,
        Err(_) => return Ed25519Result::InvalidPublicKey,
    };

    // Parse signature
    let signature = match Signature::try_from(signature_bytes) {
        Ok(sig) => sig,
        Err(_) => return Ed25519Result::InvalidSignature,
    };

    // Verify signature
    match verifying_key.verify(message_bytes, &signature) {
        Ok(_) => Ed25519Result::Success,
        Err(_) => Ed25519Result::VerificationFailed,
    }
}

#[repr(C)]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct MessageSpan {
    pub message_ptr: *const u8,
    pub message_len: usize,
}

impl MessageSpan {
    pub fn to_slice(&self) -> &[u8] {
        unsafe {
            core::slice::from_raw_parts(self.message_ptr, self.message_len)
        }
    }
}

pub unsafe extern "C" fn ed25519_dalek_verify_batch(
    public_keys: *const [u8; 32],
    signatures: *const [u8; 64],
    messages: *const MessageSpan,
    num_messages: usize,
) -> Ed25519Result {
    debug_assert!(!public_keys.is_null());
    debug_assert!(!signatures.is_null());
    debug_assert!(!messages.is_null());
    let mut pks = Vec::with_capacity(num_messages);
    let mut sigs = Vec::with_capacity(num_messages);
    let mut msgs = Vec::with_capacity(num_messages);

    for i in 0..num_messages {
        let public_key = unsafe { &*public_keys.add(i) };
        let signature = unsafe { &*signatures.add(i) };
        let message = unsafe { &*messages.add(i) };
        pks.push(match VerifyingKey::from_bytes(public_key) {
            Ok(key) => key,
            Err(_) => return Ed25519Result::InvalidPublicKey,
        });
        sigs.push(match Signature::try_from(signature) {
            Ok(sig) => sig,
            Err(_) => return Ed25519Result::InvalidSignature,
        });
        msgs.push(message.to_slice());
    }

    match verify_batch(&msgs, &sigs, &pks) {
        Ok(_) => Ed25519Result::Success,
        Err(_) => Ed25519Result::VerificationFailed,
    }
}

/// Sign a message with ed25519
///
/// # Parameters
/// - `secret_key`: 32-byte secret key array
/// - `message_ptr`: Pointer to message bytes
/// - `message_len`: Length of message in bytes
/// - `signature_out`: Pointer to 64-byte output buffer for signature
///
/// # Returns
/// Ed25519Result indicating success or failure reason
///
/// # Safety
/// Caller must ensure:
/// - secret_key points to valid 32 bytes
/// - message_ptr points to valid message_len bytes
/// - signature_out points to valid 64-byte buffer
#[no_mangle]
pub unsafe extern "C" fn ed25519_dalek_sign(
    secret_key: *const [u8; 32],
    message_ptr: *const u8,
    message_len: usize,
    signature_out: *mut [u8; 64],
) {
    debug_assert!(!secret_key.is_null());
    debug_assert!(!message_ptr.is_null());
    debug_assert!(!signature_out.is_null());

    let secret_key_bytes = unsafe { &*secret_key };
    let message_bytes =
        unsafe { core::slice::from_raw_parts(message_ptr, message_len) };
    let signature_out_bytes = unsafe { &mut *signature_out };

    // Create signing key from secret key bytes
    let signing_key = SigningKey::from_bytes(secret_key_bytes);

    // Sign the message
    let signature = signing_key.sign(message_bytes);

    // Copy signature to output buffer
    signature_out_bytes.copy_from_slice(&signature.to_bytes());
}

/// Get a human-readable description of an Ed25519Result
///
/// # Parameters
/// - `result`: The Ed25519Result to describe
///
/// # Returns
/// Pointer to a null-terminated C string describing the result.
/// The returned pointer is valid for the lifetime of the program.
#[no_mangle]
pub extern "C" fn ed25519_result_description(
    result: Ed25519Result,
) -> *const i8 {
    let description = match result {
        Ed25519Result::Success => "Success\0",
        Ed25519Result::InvalidPublicKey => "Invalid public key\0",
        Ed25519Result::InvalidSignature => "Invalid signature\0",
        Ed25519Result::VerificationFailed => "Signature verification failed\0",
        Ed25519Result::InvalidInputLength => {
            "Invalid input length or null pointer\0"
        }
    };
    description.as_ptr() as *const i8
}

#[cfg(test)]
mod tests {
    use super::*;
    use ed25519_dalek::{Signer, SigningKey};
    use rand::{rngs::OsRng, RngCore};

    #[test]
    fn test_ed25519_verify_success() {
        // Generate a test key pair
        let mut csprng = OsRng;
        let mut secret_key_bytes = [0u8; 32];
        csprng.fill_bytes(&mut secret_key_bytes);
        let signing_key = SigningKey::from_bytes(&secret_key_bytes);
        let verifying_key = signing_key.verifying_key();

        // Sign a test message
        let message = b"test message";
        let signature = signing_key.sign(message);

        // Test verification
        let result = unsafe {
            ed25519_dalek_verify(
                verifying_key.as_bytes() as *const [u8; 32],
                &signature.to_bytes() as *const [u8; 64],
                message.as_ptr(),
                message.len(),
            )
        };

        assert_eq!(result, Ed25519Result::Success);
    }

    #[test]
    fn test_ed25519_verify_invalid_signature() {
        // Generate a test key pair
        let mut csprng = OsRng;
        let mut secret_key_bytes = [0u8; 32];
        csprng.fill_bytes(&mut secret_key_bytes);
        let signing_key = SigningKey::from_bytes(&secret_key_bytes);
        let verifying_key = signing_key.verifying_key();

        // Create an invalid signature (all zeros)
        let invalid_signature = [0u8; 64];
        let message = b"test message";

        // Test verification with invalid signature
        let result = unsafe {
            ed25519_dalek_verify(
                verifying_key.as_bytes() as *const [u8; 32],
                &invalid_signature as *const [u8; 64],
                message.as_ptr(),
                message.len(),
            )
        };

        assert_eq!(result, Ed25519Result::VerificationFailed);
    }

    #[test]
    fn test_ed25519_sign_and_verify() {
        // Generate a test key pair
        let mut csprng = OsRng;
        let mut secret_key_bytes = [0u8; 32];
        csprng.fill_bytes(&mut secret_key_bytes);
        let signing_key = SigningKey::from_bytes(&secret_key_bytes);
        let verifying_key = signing_key.verifying_key();

        let message = b"test message for signing";
        let mut signature_out = [0u8; 64];

        // Test signing
        unsafe {
            ed25519_dalek_sign(
                &secret_key_bytes as *const [u8; 32],
                message.as_ptr(),
                message.len(),
                &mut signature_out as *mut [u8; 64],
            )
        };

        // Test that the signature verifies
        let verify_result = unsafe {
            ed25519_dalek_verify(
                verifying_key.as_bytes() as *const [u8; 32],
                &signature_out as *const [u8; 64],
                message.as_ptr(),
                message.len(),
            )
        };

        assert_eq!(verify_result, Ed25519Result::Success);
    }

    #[test]
    fn test_result_descriptions() {
        let desc = unsafe {
            std::ffi::CStr::from_ptr(ed25519_result_description(
                Ed25519Result::Success,
            ))
            .to_str()
            .unwrap()
        };
        assert_eq!(desc, "Success");

        let desc = unsafe {
            std::ffi::CStr::from_ptr(ed25519_result_description(
                Ed25519Result::InvalidPublicKey,
            ))
            .to_str()
            .unwrap()
        };
        assert_eq!(desc, "Invalid public key");
    }
}
