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
// Example:
// let map = SynchronizedMap::<i32, String>::new();
// let mut guard = map.get_or_insert_with(10, || "foo".to_string());
// // access to guard.value() is synchronized
// drop(guard); // release lock

use std::collections::HashMap;
use std::hash::{BuildHasher, Hash, Hasher};
use std::sync::Mutex;
use std::thread;

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

/// A guard that holds a lock on a bucket in the SynchronizedMap.
/// Dropping this guard releases the lock.
///
/// This guard provides access to a value in the map. The value reference
/// is only valid while this guard is held.
pub struct BucketGuard<'a, K, V, S = std::collections::hash_map::RandomState> {
    _guard: std::sync::MutexGuard<'a, HashMap<K, V, S>>,
    value: &'a mut V,
}

impl<'a, K, V, S> BucketGuard<'a, K, V, S> {
    /// Create a new BucketGuard from a guard and a raw pointer to the value.
    /// The raw pointer must point to a value within the HashMap guarded by `guard`.
    /// This is safe because the guard ensures the HashMap (and thus the value) remains valid.
    fn new(
        guard: std::sync::MutexGuard<'a, HashMap<K, V, S>>,
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

/// A synchronized map that provides scalable synchronization through sharded
/// hash tables with fine-grained locking.
///
/// Each bucket is protected by its own mutex, allowing concurrent access to
/// different buckets. The number of buckets must be a power of 2 for efficient
/// hash-based bucket selection.
pub struct SynchronizedMap<
    K,
    V,
    H = siphasher::sip::SipHasher24,
    S = std::collections::hash_map::RandomState,
> where
    K: Hash + Eq,
    H: Hasher + Default,
    S: BuildHasher + Default,
{
    /// Hash function state (for custom hashing)
    hasher: H,
    /// Vector of mutexes, one per bucket
    locks: Vec<Mutex<HashMap<K, V, S>>>,
}

impl<K, V> SynchronizedMap<K, V>
where
    K: Hash + Eq,
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
            locks.push(Mutex::new(HashMap::new()));
        }

        Self {
            hasher: siphasher::sip::SipHasher24::default(),
            locks,
        }
    }
}

impl<K, V, H, S> SynchronizedMap<K, V, H, S>
where
    K: Hash + Eq,
    H: Hasher + Default + Clone,
    S: BuildHasher + Default,
{
    /// Create a new SynchronizedMap with custom hash function and shard count.
    pub fn with_hasher_and_shard_count(shard_count: usize, hasher: H) -> Self {
        assert!(
            shard_count.is_power_of_two(),
            "shard_count must be a power of 2"
        );
        assert!(shard_count > 0, "shard_count must be greater than 0");

        let mut locks = Vec::with_capacity(shard_count);
        for _ in 0..shard_count {
            locks.push(Mutex::new(HashMap::with_hasher(S::default())));
        }

        Self { hasher, locks }
    }

    /// Get the bucket index for a given key.
    #[inline]
    fn get_bucket_index(&self, key: &K) -> usize {
        let mut hasher = self.hasher.clone();
        key.hash(&mut hasher);
        let hash = hasher.finish();
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
        let mut guard = self.locks[idx].lock().unwrap();
        // Get reference from entry - Entry::or_insert_with consumes the Entry and returns &mut V
        // Convert to raw pointer to avoid borrow checker issues when moving guard into BucketGuard
        let value_ref = guard.entry(key).or_insert_with(f);
        let value_ptr = value_ref as *mut V;
        BucketGuard::new(guard, value_ptr)
    }

    /// Find a value by key. Returns Some(guard) if found, None if not found.
    pub fn find(&self, key: &K) -> Option<BucketGuard<'_, K, V, S>> {
        let idx = self.get_bucket_index(key);
        let mut guard = self.locks[idx].lock().unwrap();
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
        let mut guard = self.locks[idx].lock().unwrap();
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
        let mut guard = self.locks[idx].lock().unwrap();
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
        let mut guard = self.locks[idx].lock().unwrap();
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
        let mut guard = self.locks[idx].lock().unwrap();
        guard.remove(key).is_some()
    }

    /// Get the total number of elements in the map.
    /// This requires locking all buckets, so it may be slow.
    pub fn size(&self) -> usize {
        self.locks
            .iter()
            .map(|lock| lock.lock().unwrap().len())
            .sum()
    }

    /// Clear all elements from the map.
    pub fn clear(&self) {
        for lock in &self.locks {
            lock.lock().unwrap().clear();
        }
    }

    /// Execute a function on each element.
    /// The function receives a reference to each (key, value) pair.
    pub fn for_each<F>(&self, mut f: F)
    where
        F: FnMut(&K, &V),
    {
        for lock in &self.locks {
            let guard = lock.lock().unwrap();
            for (k, v) in guard.iter() {
                f(k, v);
            }
        }
    }

    /// Execute a function on each element with mutable access.
    pub fn for_each_mut<F>(&self, mut f: F)
    where
        F: FnMut(&K, &mut V),
    {
        for lock in &self.locks {
            let mut guard = lock.lock().unwrap();
            for (k, v) in guard.iter_mut() {
                f(k, v);
            }
        }
    }
}

impl<K, V> Default for SynchronizedMap<K, V>
where
    K: Hash + Eq,
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
        let mut total = 0;
        map.for_each(|_k, v| {
            assert_eq!(*v, num_threads);
            total += *v;
        });
        assert_eq!(total, insertions * num_threads);
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
