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

/// Verify an ed25519 signature
///
/// # Parameters
/// - `public_key`: 32-byte public key array
/// - `signature`: 64-byte signature array
/// - `message_span`: MessageSpan
///
/// # Returns
/// Ed25519Result indicating success or failure reason
///
/// # Safety
/// Caller must ensure:
/// - message_span.message_ptr points to valid message_span.message_len bytes
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

    std::println!("pk: {:?}, sig: {:?}, msg: {:?}", public_key_bytes, signature_bytes, message_bytes);

    let vk_bytes: VerificationKeyBytes = (*public_key_bytes).into();

    // Parse public key
    let verifying_key: VerificationKey = match vk_bytes.try_into() {
        Ok(key) => key,
        Err(_) => return Ed25519Result::InvalidPublicKey,
    };

    // Parse signature
    let signature = Signature::from(signature_bytes);

    // Verify signature
    match verifying_key.verify(&signature, message_bytes) {
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
    public_keys: *const *const [u8; 32],
    signatures: *const *const [u8; 64],
    messages: *const MessageSpan,
    num_messages: usize,
) -> Ed25519Result {
    debug_assert!(!public_keys.is_null());
    debug_assert!(!signatures.is_null());
    debug_assert!(!messages.is_null());

    let mut batch = ed25519_zebra::batch::Verifier::new();

    for i in 0..num_messages {
        let public_key: *const [u8; 32] = unsafe { *public_keys.add(i) };
        let signature: *const [u8; 64] = unsafe { *signatures.add(i) };
        let message = unsafe { &*messages.add(i) };
        let vk_bytes: VerificationKeyBytes = (*public_key).into();

        batch.queue((
            vk_bytes,
            Signature::from(*signature),
            message.to_slice(),
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
/// - `secret_keypair`: 64-byte secret key array [public_key, secret_key]
/// - `message`: MessageSpan
/// - `signature_out`: Pointer to 64-byte output buffer for signature
///
/// # Returns
/// Ed25519Result indicating success or failure reason
///
/// # Safety
/// Caller must ensure:
/// - secret_keypair points to valid 64 bytes [public_key, secret_key]
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
    let public_key: [u8; 32] = signing_key.verification_key().into();
    public_key_out_bytes.copy_from_slice(&public_key);
    secret_keypair_out_bytes[..32].copy_from_slice(&public_key);
    secret_keypair_out_bytes[32..].copy_from_slice(&signing_key.to_bytes());
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
                &verifying_key as *const [u8; 32],
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
        let verifying_key: [u8; 32] = signing_key.verification_key().into();

        // Create an invalid signature (all zeros)
        let invalid_signature = [0u8; 64];
        let message = b"test message";

        // Test verification with invalid signature
        let result = unsafe {
            ed25519_dalek_verify(
                &verifying_key as *const [u8; 32],
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

        let mut keypair_bytes = [0u8; 64];
        let mut pk_bytes = [0u8; 32];

        unsafe {
            ed25519_dalek_keygen(
                &secret_key_bytes as *const [u8; 32],
                &mut pk_bytes as *mut [u8; 32],
                &mut keypair_bytes as *mut [u8; 64],
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
                &keypair_bytes as *const [u8; 64],
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
                &verifying_key as *const [u8; 32],
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
