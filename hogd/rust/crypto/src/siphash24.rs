use core::hash::Hasher;
use siphasher::sip::SipHasher24;

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
    key1: u64,
    key2: u64,
    data_ptr: *const u8,
    data_len: usize,
) -> u64 {
    debug_assert!(!data_ptr.is_null());

    let data_bytes = unsafe { core::slice::from_raw_parts(data_ptr, data_len) };

    // Create SipHasher24 with the key
    let mut hasher = SipHasher24::new_with_keys(key1, key2);
    hasher.write(data_bytes);
    hasher.finish()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_siphash24_basic() {
        let data = b"hello world";

        let output = unsafe { siphash24_hash(1, 1, data.as_ptr(), data.len()) };

        assert_ne!(output, 0); // Should produce some hash value
    }

    #[test]
    fn test_siphash24_different_keys_different_results() {
        let key1 = (1, 2);
        let key2 = (3, 4);
        let data = b"test data";

        let result1 = unsafe {
            siphash24_hash(key1.0, key1.1, data.as_ptr(), data.len())
        };

        let result2 = unsafe {
            siphash24_hash(key2.0, key2.1, data.as_ptr(), data.len())
        };

        assert_ne!(result1, result2); // Different keys should produce different hashes
    }

    #[test]
    fn test_siphash24_same_input_same_output() {
        let key = (42, 42);
        let data = b"consistent data";

        let result1 =
            unsafe { siphash24_hash(key.0, key.1, data.as_ptr(), data.len()) };

        let result2 =
            unsafe { siphash24_hash(key.0, key.1, data.as_ptr(), data.len()) };

        assert_eq!(result1, result2); // Same input should produce same output
    }
}
