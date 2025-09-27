use ed25519_dalek::{Signature, Verifier, VerifyingKey};

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
#[no_mangle]
pub unsafe extern "C" fn ed25519_dalek_verify(
    public_key: *const [u8; 32],
    signature: *const [u8; 64],
    message_ptr: *const u8,
    message_len: usize,
) -> Ed25519Result {
    // Validate input pointers
    if public_key.is_null() || signature.is_null() || message_ptr.is_null() {
        return Ed25519Result::InvalidInputLength;
    }

    let public_key_bytes = unsafe { &*public_key };
    let signature_bytes = unsafe { &*signature };
    let message_bytes = unsafe { core::slice::from_raw_parts(message_ptr, message_len) };

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

/// Get a human-readable description of an Ed25519Result
/// 
/// # Parameters
/// - `result`: The Ed25519Result to describe
/// 
/// # Returns
/// Pointer to a null-terminated C string describing the result.
/// The returned pointer is valid for the lifetime of the program.
#[no_mangle]
pub extern "C" fn ed25519_result_description(result: Ed25519Result) -> *const i8 {
    let description = match result {
        Ed25519Result::Success => "Success\0",
        Ed25519Result::InvalidPublicKey => "Invalid public key\0",
        Ed25519Result::InvalidSignature => "Invalid signature\0", 
        Ed25519Result::VerificationFailed => "Signature verification failed\0",
        Ed25519Result::InvalidInputLength => "Invalid input length or null pointer\0",
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
    fn test_ed25519_verify_null_pointers() {
        // Test with null pointers
        let result = unsafe {
            ed25519_dalek_verify(
                std::ptr::null(),
                std::ptr::null(),
                std::ptr::null(),
                0,
            )
        };
        
        assert_eq!(result, Ed25519Result::InvalidInputLength);
    }

    #[test]
    fn test_result_descriptions() {
        let desc = unsafe {
            std::ffi::CStr::from_ptr(ed25519_result_description(Ed25519Result::Success))
                .to_str()
                .unwrap()
        };
        assert_eq!(desc, "Success");
        
        let desc = unsafe {
            std::ffi::CStr::from_ptr(ed25519_result_description(Ed25519Result::InvalidPublicKey))
                .to_str()
                .unwrap()
        };
        assert_eq!(desc, "Invalid public key");
    }
}
