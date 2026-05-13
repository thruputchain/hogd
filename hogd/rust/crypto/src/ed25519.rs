use core::ptr;

use ed25519_zebra::{
    Signature, SigningKey, VerificationKey, VerificationKeyBytes,
};

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

fn read_fixed_32(public_key: *const u8) -> Option<[u8; 32]> {
    if public_key.is_null() {
        return None;
    }
    let mut out = [0u8; 32];
    unsafe { ptr::copy_nonoverlapping(public_key, out.as_mut_ptr(), 32) };
    Some(out)
}

fn read_fixed_64(signature: *const u8) -> Option<[u8; 64]> {
    if signature.is_null() {
        return None;
    }
    let mut out = [0u8; 64];
    unsafe { ptr::copy_nonoverlapping(signature, out.as_mut_ptr(), 64) };
    Some(out)
}

/// Verify an ed25519 signature
///
/// # Parameters
/// - `public_key`: pointer to `ED25519_PUBLIC_KEY_SIZE` contiguous key bytes
/// - `signature`: pointer to 64 contiguous signature bytes
/// - `message_span`: MessageSpan
///
/// # Returns
/// Ed25519Result indicating success or failure reason
///
/// # Safety
/// Caller must ensure:
/// - If message_span.message_len == 0, message_span.message_ptr may be null
/// - If message_span.message_len > 0, message_span.message_ptr must point to valid memory
/// - public_key points to valid 32 bytes (or null yields `InvalidInputLength`)
/// - signature points to valid 64 bytes (or null yields `InvalidInputLength`)
#[no_mangle]
pub unsafe extern "C" fn ed25519_dalek_verify(
    public_key: *const u8,
    signature: *const u8,
    message: MessageSpan,
) -> Ed25519Result {
    let (Some(public_key_bytes), Some(signature_bytes)) =
        (read_fixed_32(public_key), read_fixed_64(signature))
    else {
        return Ed25519Result::InvalidInputLength;
    };
    if message.message_len > 0 && message.message_ptr.is_null() {
        return Ed25519Result::InvalidInputLength;
    }
    let message_bytes = unsafe { message.to_slice() };

    let vk_bytes: VerificationKeyBytes = public_key_bytes.into();

    // Parse public key
    let verifying_key: VerificationKey = match vk_bytes.try_into() {
        Ok(key) => key,
        Err(_) => return Ed25519Result::InvalidPublicKey,
    };

    // Parse signature
    let signature = Signature::from(&signature_bytes);

    // Verify signature
    match verifying_key.verify(&signature, message_bytes) {
        Ok(_) => Ed25519Result::Success,
        Err(_) => Ed25519Result::VerificationFailed,
    }
}

/// Verify a batch of ed25519 signatures
///
/// # Parameters
/// - `public_keys`: Pointer to array of `num_messages` pointers; each points to 32 key bytes
/// - `signatures`: Pointer to array of `num_messages` pointers; each points to 64 signature bytes
/// - `messages`: Pointer to array of MessageSpan
/// - `num_messages`: Number of messages in the batch
///
/// # Returns
/// Ed25519Result indicating success or failure reason
///
/// # Safety
/// Caller must ensure:
/// - public_keys points to valid array of num_messages pointers to 32-byte buffers
/// - signatures points to valid array of num_messages pointers to 64-byte buffers
/// - messages points to valid array of num_messages MessageSpan
#[no_mangle]
pub unsafe extern "C" fn ed25519_dalek_verify_batch(
    public_keys: *const *const u8,
    signatures: *const *const u8,
    messages: *const MessageSpan,
    num_messages: usize,
) -> Ed25519Result {
    if public_keys.is_null() || signatures.is_null() || messages.is_null() {
        return Ed25519Result::InvalidInputLength;
    }

    let mut batch = ed25519_zebra::batch::Verifier::new();

    for i in 0..num_messages {
        let pk_ptr = unsafe { *public_keys.add(i) };
        let sig_ptr = unsafe { *signatures.add(i) };
        let message = unsafe { &*messages.add(i) };
        let (Some(public_key_bytes), Some(signature_bytes)) =
            (read_fixed_32(pk_ptr), read_fixed_64(sig_ptr))
        else {
            return Ed25519Result::InvalidInputLength;
        };
        if message.message_len > 0 && message.message_ptr.is_null() {
            return Ed25519Result::InvalidInputLength;
        }
        let vk_bytes: VerificationKeyBytes = public_key_bytes.into();
        let message_bytes = unsafe { message.to_slice() };

        batch.queue((
            vk_bytes,
            Signature::from(&signature_bytes),
            message_bytes,
        ));
    }

    match batch.verify(rand::thread_rng()) {
        Ok(_) => Ed25519Result::Success,
        Err(_) => Ed25519Result::VerificationFailed,
    }
}

/// Sign a message with ed25519
///
/// # Parameters
/// - `secret_keypair`: pointer to 64 contiguous bytes `[public_key, secret_key]`
/// - `message`: MessageSpan
/// - `signature_out`: pointer to 64-byte output buffer for signature
///
/// # Returns
/// Ed25519Result indicating success or failure reason
///
/// # Safety
/// Caller must ensure:
/// - secret_keypair points to valid 64 bytes [public_key, secret_key]
/// - If message.message_len == 0, message.message_ptr may be null
/// - If message.message_len > 0, message.message_ptr must point to valid memory
/// - signature_out points to valid 64-byte buffer
#[no_mangle]
pub unsafe extern "C" fn ed25519_dalek_sign(
    secret_keypair: *const u8,
    message: MessageSpan,
    signature_out: *mut u8,
) -> u8 {
    let Some(secret_keypair_bytes) = read_fixed_64(secret_keypair) else {
        return 1;
    };
    if signature_out.is_null() {
        return 1;
    }
    if message.message_len > 0 && message.message_ptr.is_null() {
        return 1;
    }
    let message_bytes = unsafe { message.to_slice() };

    // Create signing key from secret keypair bytes
    let signing_key =
        SigningKey::from_bytes(&secret_keypair_bytes[32..].try_into().unwrap());

    let vk_bytes: VerificationKeyBytes = signing_key.verification_key().into();
    let vk_bytes_expected: &[u8; 32] =
        &secret_keypair_bytes[..32].try_into().unwrap();

    if vk_bytes != (*vk_bytes_expected).into() {
        return 1;
    }

    // Sign the message
    let signature = signing_key.sign(message_bytes);

    // Copy signature to output buffer
    unsafe {
        ptr::copy_nonoverlapping(
            signature.to_bytes().as_ptr(),
            signature_out,
            64,
        );
    }
    0
}

/// Generate a new ed25519 keypair given a random seed
///
/// # Parameters
/// - `random_bytes`: pointer to 32 random seed bytes
/// - `public_key_out`: pointer to 32-byte output buffer for public key
/// - `secret_keypair_out`: pointer to 64-byte output buffer for secret key
///
/// # Safety
/// Caller must ensure:
/// - random_bytes points to valid 32 bytes
/// - public_key_out points to valid 32 bytes
/// - secret_keypair_out points to valid 64 bytes
#[no_mangle]
pub unsafe extern "C" fn ed25519_dalek_keygen(
    random_bytes: *const u8,
    public_key_out: *mut u8,
    secret_keypair_out: *mut u8,
) {
    let Some(random_bytes_bytes) = read_fixed_32(random_bytes) else {
        return;
    };
    if public_key_out.is_null() || secret_keypair_out.is_null() {
        return;
    }

    debug_assert_eq!(ED25519_SECRET_KEYPAIR_SIZE, 64);
    debug_assert_eq!(ED25519_PUBLIC_KEY_SIZE, 32);

    let signing_key = SigningKey::from_bytes(&random_bytes_bytes);
    let public_key: [u8; 32] = signing_key.verification_key().into();
    unsafe {
        ptr::copy_nonoverlapping(public_key.as_ptr(), public_key_out, 32);
        ptr::copy_nonoverlapping(public_key.as_ptr(), secret_keypair_out, 32);
        ptr::copy_nonoverlapping(
            signing_key.to_bytes().as_ptr(),
            secret_keypair_out.add(32),
            32,
        );
    }
}

pub const ED25519_SECRET_KEYPAIR_SIZE: usize = 64;
pub const ED25519_PUBLIC_KEY_SIZE: usize = 32;

#[cfg(test)]
mod tests {
    use super::*;
    use ed25519_zebra::SigningKey;
    use rand::{rngs::OsRng, RngCore};

    #[test]
    fn test_ed25519_verify_success() {
        // Generate a test key pair
        let mut csprng = OsRng;
        let mut secret_key_bytes = [0u8; 32];
        csprng.fill_bytes(&mut secret_key_bytes);
        let signing_key = SigningKey::from_bytes(&secret_key_bytes);
        let verifying_key: [u8; 32] = signing_key.verification_key().into();

        // Sign a test message
        let message = b"test message";
        let signature = signing_key.sign(message);

        // Test verification
        let result = unsafe {
            ed25519_dalek_verify(
                verifying_key.as_ptr(),
                signature.to_bytes().as_ptr(),
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
        let verifying_key: [u8; 32] = signing_key.verification_key().into();

        // Create an invalid signature (all zeros)
        let invalid_signature = [0u8; 64];
        let message = b"test message";

        // Test verification with invalid signature
        let result = unsafe {
            ed25519_dalek_verify(
                verifying_key.as_ptr(),
                invalid_signature.as_ptr(),
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

        let mut keypair_bytes = [0u8; 64];
        let mut pk_bytes = [0u8; 32];

        unsafe {
            ed25519_dalek_keygen(
                secret_key_bytes.as_ptr(),
                pk_bytes.as_mut_ptr(),
                keypair_bytes.as_mut_ptr(),
            )
        };

        let signing_key =
            SigningKey::from_bytes(&keypair_bytes[32..].try_into().unwrap());
        let verifying_key: [u8; 32] = signing_key.verification_key().into();

        assert_eq!(verifying_key, pk_bytes);

        let message = b"test message for signing";
        let mut signature_out = [0u8; 64];

        // Test signing
        unsafe {
            ed25519_dalek_sign(
                keypair_bytes.as_ptr(),
                MessageSpan {
                    message_ptr: message.as_ptr(),
                    message_len: message.len(),
                },
                signature_out.as_mut_ptr(),
            )
        };

        // Test that the signature verifies
        let verify_result = unsafe {
            ed25519_dalek_verify(
                verifying_key.as_ptr(),
                signature_out.as_ptr(),
                MessageSpan {
                    message_ptr: message.as_ptr(),
                    message_len: message.len(),
                },
            )
        };

        assert_eq!(verify_result, Ed25519Result::Success);
    }

    #[test]
    fn test_ed25519_verify_empty_message() {
        let mut csprng = OsRng;
        let mut secret_key_bytes = [0u8; 32];
        csprng.fill_bytes(&mut secret_key_bytes);
        let signing_key = SigningKey::from_bytes(&secret_key_bytes);
        let verifying_key: [u8; 32] = signing_key.verification_key().into();

        let fake_signature = [0u8; 64];

        let result = unsafe {
            ed25519_dalek_verify(
                verifying_key.as_ptr(),
                fake_signature.as_ptr(),
                MessageSpan {
                    message_ptr: std::ptr::null(),
                    message_len: 0,
                },
            )
        };

        assert_eq!(result, Ed25519Result::VerificationFailed);
    }

    #[test]
    fn test_ed25519_sign_empty_message() {
        let mut csprng = OsRng;
        let mut secret_key_bytes = [0u8; 32];
        csprng.fill_bytes(&mut secret_key_bytes);

        let mut keypair_bytes = [0u8; 64];
        let mut pk_bytes = [0u8; 32];

        unsafe {
            ed25519_dalek_keygen(
                secret_key_bytes.as_ptr(),
                pk_bytes.as_mut_ptr(),
                keypair_bytes.as_mut_ptr(),
            )
        };

        let mut signature_out = [0u8; 64];

        let result = unsafe {
            ed25519_dalek_sign(
                keypair_bytes.as_ptr(),
                MessageSpan {
                    message_ptr: std::ptr::null(),
                    message_len: 0,
                },
                signature_out.as_mut_ptr(),
            )
        };

        assert_eq!(result, 0);
    }
}
