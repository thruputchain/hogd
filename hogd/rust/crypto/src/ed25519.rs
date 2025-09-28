use ed25519_dalek::{
    verify_batch, Signature, Signer, SigningKey, Verifier, VerifyingKey,
};

extern crate alloc;
use alloc::vec::Vec;

use crate::MessageSpan;

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
    message: MessageSpan,
) -> Ed25519Result {
    debug_assert!(!public_key.is_null());
    debug_assert!(!signature.is_null());
    debug_assert!(!message.message_ptr.is_null());

    let public_key_bytes = unsafe { &*public_key };
    let signature_bytes = unsafe { &*signature };
    let message_bytes = message.to_slice();

    // Parse public key
    let verifying_key = match VerifyingKey::from_bytes(public_key_bytes) {
        Ok(key) => key,
        Err(_) => return Ed25519Result::InvalidPublicKey,
    };

    // Parse signature
    let signature = Signature::from(signature_bytes);

    // Verify signature
    match verifying_key.verify(message_bytes, &signature) {
        Ok(_) => Ed25519Result::Success,
        Err(_) => Ed25519Result::VerificationFailed,
    }
}

/// Verify a batch of ed25519 signatures
///
/// # Parameters
/// - `public_keys`: Pointer to array of 32-byte public keys
/// - `signatures`: Pointer to array of 64-byte signatures
/// - `messages`: Pointer to array of MessageSpan
/// - `num_messages`: Number of messages in the batch
///
/// # Returns
/// Ed25519Result indicating success or failure reason
///
/// # Safety
/// Caller must ensure:
/// - public_keys points to valid array of num_messages keys, each 32 bytes
/// - signatures points to valid array of num_messages signatures, each 64 bytes
/// - messages points to valid array of num_messages MessageSpan
#[no_mangle]
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
        sigs.push(Signature::from(signature));
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
/// - `secret_keypair`: 64-byte secret key array
/// - `message`: MessageSpan
/// - `signature_out`: Pointer to 64-byte output buffer for signature
///
/// # Returns
/// Ed25519Result indicating success or failure reason
///
/// # Safety
/// Caller must ensure:
/// - secret_keypair points to valid 64 bytes
/// - message.message_ptr points to valid message.message_len bytes
/// - signature_out points to valid 64-byte buffer
#[no_mangle]
pub unsafe extern "C" fn ed25519_dalek_sign(
    secret_keypair: *const [u8; 64],
    message: MessageSpan,
    signature_out: *mut [u8; 64],
) -> u8 {
    debug_assert!(!secret_keypair.is_null());
    debug_assert!(!message.message_ptr.is_null());
    debug_assert!(!signature_out.is_null());

    let secret_keypair_bytes = unsafe { &*secret_keypair };
    let message_bytes = message.to_slice();
    let signature_out_bytes = unsafe { &mut *signature_out };

    // Create signing key from secret keypair bytes
    let signing_key = match SigningKey::from_keypair_bytes(secret_keypair_bytes)
    {
        Ok(key) => key,
        Err(_) => {
            return 1;
        }
    };

    // Sign the message
    let signature = signing_key.sign(message_bytes);

    // Copy signature to output buffer
    signature_out_bytes.copy_from_slice(&signature.to_bytes());
    0
}

/// Generate a new ed25519 keypair given a random seed
///
/// # Parameters
/// - `random_bytes`: Pointer to 32-byte random bytes
/// - `public_key_out`: Pointer to 32-byte output buffer for public key
/// - `secret_keypair_out`: Pointer to 64-byte output buffer for secret key
///
/// # Safety
/// Caller must ensure:
/// - random_bytes points to valid 32 bytes
/// - public_key_out points to valid 32 bytes
/// - secret_keypair_out points to valid 64 bytes
#[no_mangle]
pub unsafe extern "C" fn ed25519_dalek_keygen(
    random_bytes: *const [u8; 32],
    public_key_out: *mut [u8; 32],
    secret_keypair_out: *mut [u8; 64],
) {
    debug_assert!(!random_bytes.is_null());
    debug_assert!(!public_key_out.is_null());
    debug_assert!(!secret_keypair_out.is_null());

    debug_assert_eq!(ED25519_SECRET_KEYPAIR_SIZE, 64);
    debug_assert_eq!(ED25519_PUBLIC_KEY_SIZE, 32);

    let random_bytes_bytes = unsafe { &*random_bytes };
    let public_key_out_bytes = unsafe { &mut *public_key_out };
    let secret_keypair_out_bytes = unsafe { &mut *secret_keypair_out };

    let signing_key = SigningKey::from_bytes(random_bytes_bytes);
    let public_key = signing_key.verifying_key().to_bytes();
    public_key_out_bytes.copy_from_slice(&public_key);
    secret_keypair_out_bytes.copy_from_slice(&signing_key.to_keypair_bytes());
}

pub const ED25519_SECRET_KEYPAIR_SIZE: usize = 64;
pub const ED25519_PUBLIC_KEY_SIZE: usize = 32;

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
                MessageSpan {
                    message_ptr: message.as_ptr(),
                    message_len: message.len(),
                },
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
                MessageSpan {
                    message_ptr: message.as_ptr(),
                    message_len: message.len(),
                },
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

        let signing_key_bytes = signing_key.to_keypair_bytes();

        let message = b"test message for signing";
        let mut signature_out = [0u8; 64];

        // Test signing
        unsafe {
            ed25519_dalek_sign(
                &signing_key_bytes as *const [u8; 64],
                MessageSpan {
                    message_ptr: message.as_ptr(),
                    message_len: message.len(),
                },
                &mut signature_out as *mut [u8; 64],
            )
        };

        // Test that the signature verifies
        let verify_result = unsafe {
            ed25519_dalek_verify(
                verifying_key.as_bytes() as *const [u8; 32],
                &signature_out as *const [u8; 64],
                MessageSpan {
                    message_ptr: message.as_ptr(),
                    message_len: message.len(),
                },
            )
        };

        assert_eq!(verify_result, Ed25519Result::Success);
    }
}
