// synchronized_map - provides a bucket hash table that exposes locking
//
// SynchronizedMap is similar to a std::collections::HashMap, but provides scalable
// synchronization through a sharded hash table with buckets that are each
// protected by a separate lock.
//
// SynchronizedMap exposes these locks through a MutexGuard in many of
// its operations, allowing the caller to control when the lock is released so
// it can synchronize access to the map's elements.
//
// API design is largely mirrored from hogd/base/synchronized_map.h,
// with some of the features there stripped out (i.e. parallel iteration).
//
// Example:
// let map = SynchronizedMap::<i32, String>::new();
// let mut guard = map.get_or_insert_with(10, || "foo".to_string());
// // access to guard.value() is synchronized
// drop(guard); // release lock

use std::collections::HashMap;
use std::hash::{BuildHasher, Hash, Hasher};
use std::sync::RwLock;
use std::thread;

use siphasher::sip::SipHasher24;

/// Default shard base order (log2 of shards per core)
const DEFAULT_SHARD_BASE_ORDER: u32 = 9;

/// Returns the number of shards needed for good multicore scalability.
/// The number of shards is a power of 2, calculated as 2^(base_order + log2(cores)).
fn scalable_shard_size(base_order: u32) -> usize {
    let cores = thread::available_parallelism()
        .map(|n| n.get())
        .unwrap_or(1);
    let order = base_order + cores.ilog2();
    1usize << order
}

/// A guard that holds a write lock on a bucket in the SynchronizedMap.
/// Dropping this guard releases the lock.
///
/// This guard provides mutable access to a value in the map. The value reference
/// is only valid while this guard is held.
pub struct BucketGuard<'a, K, V, S = std::collections::hash_map::RandomState> {
    _guard: std::sync::RwLockWriteGuard<'a, HashMap<K, V, S>>,
    value: &'a mut V,
}

impl<'a, K, V, S> BucketGuard<'a, K, V, S> {
    /// Create a new BucketGuard from a write guard and a raw pointer to the value.
    /// The raw pointer must point to a value within the HashMap guarded by `guard`.
    /// This is safe because the guard ensures the HashMap (and thus the value) remains valid.
    fn new(
        guard: std::sync::RwLockWriteGuard<'a, HashMap<K, V, S>>,
        value_ptr: *mut V,
    ) -> Self {
        // SAFETY: The raw pointer points to a value within the HashMap guarded by `guard`.
        // The guard ensures the HashMap remains valid for the lifetime 'a, so the value is also valid.
        let value = unsafe { &mut *value_ptr };
        Self {
            _guard: guard,
            value,
        }
    }

    /// Get a mutable reference to the value.
    /// The reference is valid as long as this guard is held.
    pub fn value(&mut self) -> &mut V {
        self.value
    }
}

/// A guard that holds a read lock on a bucket in the SynchronizedMap.
/// Dropping this guard releases the lock.
///
/// This guard provides immutable access to a value in the map. Multiple read guards
/// can be held concurrently on the same bucket.
pub struct BucketReadGuard<
    'a,
    K,
    V,
    S = std::collections::hash_map::RandomState,
> {
    _guard: std::sync::RwLockReadGuard<'a, HashMap<K, V, S>>,
    value: &'a V,
}

impl<'a, K, V, S> BucketReadGuard<'a, K, V, S> {
    /// Create a new BucketReadGuard from a read guard and a raw pointer to the value.
    fn new(
        guard: std::sync::RwLockReadGuard<'a, HashMap<K, V, S>>,
        value_ptr: *const V,
    ) -> Self {
        // SAFETY: The raw pointer points to a value within the HashMap guarded by `guard`.
        // The guard ensures the HashMap remains valid for the lifetime 'a, so the value is also valid.
        let value = unsafe { &*value_ptr };
        Self {
            _guard: guard,
            value,
        }
    }

    /// Get an immutable reference to the value.
    /// The reference is valid as long as this guard is held.
    pub fn value(&self) -> &V {
        self.value
    }
}

/// A synchronized map that provides scalable synchronization through sharded
/// hash tables with fine-grained locking.
///
/// Each bucket is protected by its own RwLock, allowing concurrent read access to
/// the same bucket and exclusive write access. The number of buckets must be a
/// power of 2 for efficient hash-based bucket selection.
// The Send + Sync bounds on K, V, S are part of the type's contract: a
// SynchronizedMap is meant to be shared across threads, and the
// underlying RwLock<HashMap<K, V, S>> is only Send + Sync when K, V, S
// are. Encoding the bounds on the struct itself rules out constructing
// a !Send/!Sync instantiation at all, which is stronger than a single
// compile-time assertion at one use site.
pub struct SynchronizedMap<K, V, S = std::collections::hash_map::RandomState>
where
    K: Hash + Eq + Send + Sync,
    V: Send + Sync,
    S: BuildHasher + Default + Send + Sync,
{
    /// Vector of RwLocks, one per bucket
    locks: Vec<RwLock<HashMap<K, V, S>>>,

    /// Siphash24 keys
    siphash24_keys: [u64; 2],
}

impl<K, V> SynchronizedMap<K, V>
where
    K: Hash + Eq + Send + Sync,
    V: Send + Sync,
{
    /// Create a new SynchronizedMap with a default number of shards.
    pub fn new() -> Self {
        Self::with_shard_count(scalable_shard_size(DEFAULT_SHARD_BASE_ORDER))
    }

    /// Create a new SynchronizedMap with a specific number of shards.
    /// The shard count must be a power of 2.
    pub fn with_shard_count(shard_count: usize) -> Self {
        assert!(
            shard_count.is_power_of_two(),
            "shard_count must be a power of 2"
        );
        assert!(shard_count > 0, "shard_count must be greater than 0");

        let mut locks = Vec::with_capacity(shard_count);
        for _ in 0..shard_count {
            locks.push(RwLock::new(HashMap::new()));
        }

        // Initialize SipHash24 keys with random values for security
        let mut siphash24_keys = [0u64; 2];
        let mut key_bytes = [0u8; 16]; // 2 * u64 = 16 bytes
        getrandom::fill(&mut key_bytes).unwrap();
        // Convert bytes to u64 values (little-endian)
        siphash24_keys[0] =
            u64::from_le_bytes(key_bytes[0..8].try_into().unwrap());
        siphash24_keys[1] =
            u64::from_le_bytes(key_bytes[8..16].try_into().unwrap());

        Self {
            locks,
            siphash24_keys,
        }
    }
}

impl<K, V, S> SynchronizedMap<K, V, S>
where
    K: Hash + Eq + Send + Sync,
    V: Send + Sync,
    S: BuildHasher + Default + Send + Sync,
{
    /// Get the bucket index for a given key.
    #[inline]
    fn get_bucket_index(&self, key: &K) -> usize {
        let mut hasher = SipHasher24::new_with_keys(
            self.siphash24_keys[0],
            self.siphash24_keys[1],
        );
        key.hash(&mut hasher);
        let hash = hasher.finish();
        debug_assert!(
            self.locks.len().is_power_of_two(),
            "locks.len() must be a power of 2"
        );

        (hash as usize) & (self.locks.len() - 1)
    }

    /// Get a mutable reference to a value, creating it if it doesn't exist.
    /// Returns a guard that provides access to the value.
    ///
    /// This is similar to the C++ `operator[]` behavior.
    pub fn get_or_insert_with<F>(
        &self,
        key: K,
        f: F,
    ) -> BucketGuard<'_, K, V, S>
    where
        F: FnOnce() -> V,
    {
        let idx = self.get_bucket_index(&key);
        let mut guard = self.locks[idx].write().unwrap();
        // Get reference from entry - Entry::or_insert_with consumes the Entry and returns &mut V
        // Convert to raw pointer to avoid borrow checker issues when moving guard into BucketGuard
        let value_ref = guard.entry(key).or_insert_with(f);
        let value_ptr = value_ref as *mut V;
        BucketGuard::new(guard, value_ptr)
    }

    /// Find a value by key (read-only). Returns Some(guard) if found, None if not found.
    /// Multiple readers can hold read guards on the same bucket concurrently.
    pub fn find_read(&self, key: &K) -> Option<BucketReadGuard<'_, K, V, S>> {
        let idx = self.get_bucket_index(key);
        let guard = self.locks[idx].read().unwrap();
        if let Some(value_ref) = guard.get(key) {
            let value_ptr = value_ref as *const V;
            Some(BucketReadGuard::new(guard, value_ptr))
        } else {
            None
        }
    }

    /// Find a value by key (mutable). Returns Some(guard) if found, None if not found.
    pub fn find(&self, key: &K) -> Option<BucketGuard<'_, K, V, S>> {
        let idx = self.get_bucket_index(key);
        let mut guard = self.locks[idx].write().unwrap();
        if let Some(value_ref) = guard.get_mut(key) {
            let value_ptr = value_ref as *mut V;
            Some(BucketGuard::new(guard, value_ptr))
        } else {
            None
        }
    }

    /// Insert a value, returning a guard to the inserted value if insertion succeeded,
    /// or None if the key already existed.
    pub fn insert(&self, key: K, value: V) -> Option<BucketGuard<'_, K, V, S>> {
        let idx = self.get_bucket_index(&key);
        let mut guard = self.locks[idx].write().unwrap();
        match guard.entry(key) {
            std::collections::hash_map::Entry::Occupied(_) => None,
            std::collections::hash_map::Entry::Vacant(entry) => {
                let value_ref = entry.insert(value);
                let value_ptr = value_ref as *mut V;
                Some(BucketGuard::new(guard, value_ptr))
            }
        }
    }

    /// Find a value or insert a default if not found.
    /// Always returns a guard to the value.
    pub fn find_or_insert(
        &self,
        key: K,
        default: V,
    ) -> BucketGuard<'_, K, V, S> {
        let idx = self.get_bucket_index(&key);
        let mut guard = self.locks[idx].write().unwrap();
        let value_ref = guard.entry(key).or_insert(default);
        let value_ptr = value_ref as *mut V;
        BucketGuard::new(guard, value_ptr)
    }

    /// Insert or assign a value. Returns a guard to the value.
    pub fn insert_or_assign(
        &self,
        key: K,
        value: V,
    ) -> BucketGuard<'_, K, V, S> {
        let idx = self.get_bucket_index(&key);
        let mut guard = self.locks[idx].write().unwrap();
        match guard.entry(key) {
            std::collections::hash_map::Entry::Occupied(mut entry) => {
                *entry.get_mut() = value;
                let value_ptr = entry.into_mut() as *mut V;
                BucketGuard::new(guard, value_ptr)
            }
            std::collections::hash_map::Entry::Vacant(entry) => {
                let value_ref = entry.insert(value);
                let value_ptr = value_ref as *mut V;
                BucketGuard::new(guard, value_ptr)
            }
        }
    }

    /// Remove a value by key. Returns true if the key was present.
    pub fn remove(&self, key: &K) -> bool {
        let idx = self.get_bucket_index(key);
        let mut guard = self.locks[idx].write().unwrap();
        guard.remove(key).is_some()
    }

    /// Get the total number of elements in the map.
    /// This requires locking all buckets, so it may be slow.
    pub fn size(&self) -> usize {
        self.locks
            .iter()
            .map(|lock| lock.read().unwrap().len())
            .sum()
    }

    /// Clear all elements from the map.
    pub fn clear(&self) {
        for lock in &self.locks {
            lock.write().unwrap().clear();
        }
    }
}

impl<K, V> Default for SynchronizedMap<K, V>
where
    K: Hash + Eq + Send + Sync,
    V: Send + Sync,
{
    fn default() -> Self {
        Self::new()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_basic_usage() {
        let map = SynchronizedMap::<i32, String>::new();

        // Insert and retrieve
        {
            let mut guard = map.get_or_insert_with(10, || "foo".to_string());
            assert_eq!(guard.value(), "foo");
        }

        {
            let mut guard = map.get_or_insert_with(20, || "bar".to_string());
            assert_eq!(guard.value(), "bar");
        }

        // Modify
        {
            let mut guard = map.get_or_insert_with(10, || String::new());
            *guard.value() = "baz".to_string();
        }

        // Recheck
        {
            let mut guard = map.get_or_insert_with(10, || String::new());
            assert_eq!(guard.value(), "baz");
        }

        // Can't reinsert
        {
            let result = map.insert(10, "foo".to_string());
            assert!(result.is_none());
        }

        // Test removal
        assert_eq!(map.size(), 2);
        assert!(map.remove(&10));
        assert_eq!(map.size(), 1);
        {
            let result = map.find(&10);
            assert!(result.is_none());
        }
    }

    #[test]
    fn test_find_or_insert() {
        let map = SynchronizedMap::<i32, i32>::new();

        {
            let mut guard = map.find_or_insert(42, 100);
            assert_eq!(*guard.value(), 100);
        }

        {
            let mut guard = map.find_or_insert(42, 200);
            assert_eq!(*guard.value(), 100); // Should still be 100
        }
    }

    #[test]
    fn test_insert_or_assign() {
        let map = SynchronizedMap::<i32, i32>::new();

        {
            let mut guard = map.insert_or_assign(42, 100);
            assert_eq!(*guard.value(), 100);
        }

        {
            let mut guard = map.insert_or_assign(42, 200);
            assert_eq!(*guard.value(), 200); // Should be updated to 200
        }
    }

    #[test]
    fn test_parallel_access() {
        use std::sync::Arc;
        use std::thread;

        // Use a smaller shard count to ensure keys hash to same buckets
        let map = Arc::new(SynchronizedMap::<i32, i32>::with_shard_count(8));
        let num_threads = 16;
        let insertions = 1024;

        let mut handles = vec![];
        for _ in 0..num_threads {
            let map = Arc::clone(&map);
            handles.push(thread::spawn(move || {
                for i in 0..insertions {
                    {
                        let mut guard = map.get_or_insert_with(i, || 0);
                        *guard.value() += 1;
                    } // Explicitly drop guard here
                }
            }));
        }

        for handle in handles {
            handle.join().unwrap();
        }

        assert_eq!(map.size(), insertions as usize);

        for i in 0..insertions {
            let mut guard = map.find(&i).unwrap();
            assert_eq!(*guard.value(), num_threads);
        }
    }

    #[test]
    fn test_clear() {
        let map = SynchronizedMap::<i32, String>::new();

        {
            let _guard1 = map.get_or_insert_with(1, || "a".to_string());
            let _guard2 = map.get_or_insert_with(2, || "b".to_string());
            let _guard3 = map.get_or_insert_with(3, || "c".to_string());
        }

        assert_eq!(map.size(), 3);
        map.clear();
        assert_eq!(map.size(), 0);
    }

    #[test]
    fn test_custom_shard_count() {
        let map = SynchronizedMap::<i32, String>::with_shard_count(8);
        {
            let _guard = map.get_or_insert_with(1, || "test".to_string());
        }
        assert_eq!(map.size(), 1);
    }

    #[test]
    #[should_panic(expected = "power of 2")]
    fn test_invalid_shard_count() {
        SynchronizedMap::<i32, String>::with_shard_count(7);
    }
}
