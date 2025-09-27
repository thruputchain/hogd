use siphasher::sip::SipHasher24;
use std::hash::{Hash, Hasher};


/// Compute SipHash24 with the given key and data
///
/// # Parameters
/// - `key`: 16-byte SipHash24 key
/// - `data_ptr`: Pointer to data bytes
/// - `data_len`: Length of data in bytes
///
/// # Returns
/// u64 hash value
///
/// # Safety
/// Caller must ensure:
/// - key points to valid 16 bytes
/// - data_ptr points to valid data_len bytes
#[no_mangle]
pub unsafe extern "C" fn siphash24_hash(
    key: *const [u8; 16],
    data_ptr: *const u8,
    data_len: usize,
) -> u64 {
    debug_assert!(!key.is_null());
    debug_assert!(!data_ptr.is_null());

    let key_bytes = unsafe { &*key };
    let data_bytes = unsafe { core::slice::from_raw_parts(data_ptr, data_len) };

    // Create SipHasher24 with the key
    let mut hasher = SipHasher24::new_with_key(key_bytes);
    data_bytes.hash(&mut hasher);
    hasher.finish()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_siphash24_basic() {
        let key = [1u8; 16];
        let data = b"hello world";
        let mut output = 0u64;

        let result = unsafe {
            siphash24_hash(
                &key as *const SipHash24KeyFFI,
                data.as_ptr(),
                data.len(),
                &mut output as *mut u64,
            )
        };

        assert_eq!(result, SipHash24Result::Success);
        assert_ne!(output, 0); // Should produce some hash value
    }

    #[test]
    fn test_siphash24_different_keys_different_results() {
        let key1 = [1u8; 16];
        let key2 = [2u8; 16];
        let data = b"test data";
        let mut output1 = 0u64;
        let mut output2 = 0u64;

        let result1 = unsafe {
            siphash24_hash(
                &key1 as *const SipHash24KeyFFI,
                data.as_ptr(),
                data.len(),
                &mut output1 as *mut u64,
            )
        };

        let result2 = unsafe {
            siphash24_hash(
                &key2 as *const SipHash24KeyFFI,
                data.as_ptr(),
                data.len(),
                &mut output2 as *mut u64,
            )
        };

        assert_eq!(result1, SipHash24Result::Success);
        assert_eq!(result2, SipHash24Result::Success);
        assert_ne!(output1, output2); // Different keys should produce different hashes
    }

    #[test]
    fn test_siphash24_same_input_same_output() {
        let key = [42u8; 16];
        let data = b"consistent data";
        let mut output1 = 0u64;
        let mut output2 = 0u64;

        let result1 = unsafe {
            siphash24_hash(
                &key as *const SipHash24KeyFFI,
                data.as_ptr(),
                data.len(),
                &mut output1 as *mut u64,
            )
        };

        let result2 = unsafe {
            siphash24_hash(
                &key as *const SipHash24KeyFFI,
                data.as_ptr(),
                data.len(),
                &mut output2 as *mut u64,
            )
        };

        assert_eq!(result1, SipHash24Result::Success);
        assert_eq!(result2, SipHash24Result::Success);
        assert_eq!(output1, output2); // Same input should produce same output
    }
}
