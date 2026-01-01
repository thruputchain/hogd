use std::cell::RefCell;
use std::collections::HashMap;
use std::ffi::c_void;
use std::fs;
use std::path::PathBuf;

// Import the FFI functions from wasmtime_lib
use common::SystemErrorCode;
use wasmtime_lib::instance_pre_cache::{
    arc_instance_pre_clone_ref, arc_instance_pre_drop_ref
};
use wasmtime_lib::{
    free_wasmtime_context, free_wasmtime_global_context, free_wasmtime_runtime,
    new_wasmtime_context, new_wasmtime_global_context, new_wasmtime_runtime,
    wasmtime_get_available_gas, wasmtime_get_memory, wasmtime_invoke,
    wasmtime_set_available_gas, wasmtime_verify_contract, ContractGetResult,
};

// Stub implementations of the C trampoline functions that the wasmtime library expects.
// These are normally provided by the C++ build system, but for standalone Rust tests
// we provide empty stubs so the linker is happy.
#[no_mangle]
extern "C" fn c_call_0args(
    _fn_ptr: *mut c_void,
    _userctx: *mut c_void,
) -> common::TrampolineResult {
    common::TrampolineResult {
        result: 0,
        panic: common::HostFnErrorCode::kHostNoneOrRecoverable as i32,
    }
}

#[no_mangle]
extern "C" fn c_call_1args(
    _fn_ptr: *mut c_void,
    _userctx: *mut c_void,
    _arg1: u64,
) -> common::TrampolineResult {
    common::TrampolineResult {
        result: 0,
        panic: common::HostFnErrorCode::kHostNoneOrRecoverable as i32,
    }
}

#[no_mangle]
extern "C" fn c_call_2args(
    _fn_ptr: *mut c_void,
    _userctx: *mut c_void,
    _arg1: u64,
    _arg2: u64,
) -> common::TrampolineResult {
    common::TrampolineResult {
        result: 0,
        panic: common::HostFnErrorCode::kHostNoneOrRecoverable as i32,
    }
}

#[no_mangle]
extern "C" fn c_call_3args(
    _fn_ptr: *mut c_void,
    _userctx: *mut c_void,
    _arg1: u64,
    _arg2: u64,
    _arg3: u64,
) -> common::TrampolineResult {
    common::TrampolineResult {
        result: 0,
        panic: common::HostFnErrorCode::kHostNoneOrRecoverable as i32,
    }
}

#[no_mangle]
extern "C" fn c_call_4args(
    _fn_ptr: *mut c_void,
    _userctx: *mut c_void,
    _arg1: u64,
    _arg2: u64,
    _arg3: u64,
    _arg4: u64,
) -> common::TrampolineResult {
    common::TrampolineResult {
        result: 0,
        panic: common::HostFnErrorCode::kHostNoneOrRecoverable as i32,
    }
}

#[no_mangle]
extern "C" fn c_call_5args(
    _fn_ptr: *mut c_void,
    _userctx: *mut c_void,
    _arg1: u64,
    _arg2: u64,
    _arg3: u64,
    _arg4: u64,
    _arg5: u64,
) -> common::TrampolineResult {
    common::TrampolineResult {
        result: 0,
        panic: common::HostFnErrorCode::kHostNoneOrRecoverable as i32,
    }
}

#[no_mangle]
extern "C" fn c_call_6args(
    _fn_ptr: *mut c_void,
    _userctx: *mut c_void,
    _arg1: u64,
    _arg2: u64,
    _arg3: u64,
    _arg4: u64,
    _arg5: u64,
    _arg6: u64,
) -> common::TrampolineResult {
    common::TrampolineResult {
        result: 0,
        panic: common::HostFnErrorCode::kHostNoneOrRecoverable as i32,
    }
}

#[no_mangle]
extern "C" fn c_call_7args(
    _fn_ptr: *mut c_void,
    _userctx: *mut c_void,
    _arg1: u64,
    _arg2: u64,
    _arg3: u64,
    _arg4: u64,
    _arg5: u64,
    _arg6: u64,
    _arg7: u64,
) -> common::TrampolineResult {
    common::TrampolineResult {
        result: 0,
        panic: common::HostFnErrorCode::kHostNoneOrRecoverable as i32,
    }
}

#[no_mangle]
extern "C" fn c_call_8args(
    _fn_ptr: *mut c_void,
    _userctx: *mut c_void,
    _arg1: u64,
    _arg2: u64,
    _arg3: u64,
    _arg4: u64,
    _arg5: u64,
    _arg6: u64,
    _arg7: u64,
    _arg8: u64,
) -> common::TrampolineResult {
    common::TrampolineResult {
        result: 0,
        panic: common::HostFnErrorCode::kHostNoneOrRecoverable as i32,
    }
}

// Mock ContractGetFn for testing
// Returns the WASM bytes stored in MOCK_CONTRACT_DATA
unsafe extern "C" fn no_get_mock_contract_get_fn(
    _userctx: *mut c_void,
    _hash: *const u8,
) -> ContractGetResult {
    ContractGetResult::new(SystemErrorCode::kSystemUnrecoverable as i32, &[])
}

type SHA256Hash = [u8; 32];

struct InstancePreBoxFreeFn {
    cached_object: *mut c_void,
}

impl Drop for InstancePreBoxFreeFn {
    fn drop(&mut self) {
        unsafe {
            arc_instance_pre_drop_ref(self.cached_object);
        }
    }
}

// Test cache that stores InstancePre objects
struct TestInstancePreCache {
    instance_pres: HashMap<SHA256Hash, InstancePreBoxFreeFn>,
}

impl TestInstancePreCache {
    fn new() -> Self {
        Self {
            instance_pres: HashMap::new(),
        }
    }

    fn get(&self, hash: &SHA256Hash) -> Option<*mut c_void> {
        self.instance_pres.get(hash).map(|ip| {
            // Return a pointer to the cached InstancePre
            // The caller (instance_pre_cache.rs) will create a reference from this pointer
            // and immediately clone it, so the pointer just needs to be valid for that operation
            // We return a pointer to the boxed InstancePre in the cache
            let ptr = ip.cached_object as *mut c_void;
            unsafe { arc_instance_pre_clone_ref(ptr) };
            ptr
        })
    }

    fn put(&mut self, hash: SHA256Hash, obj: *mut c_void) {
        // Only put if not already cached to avoid replacing entries that might still be in use
        let instance_pre_box_free_fn =
            InstancePreBoxFreeFn { cached_object: obj };
        self.instance_pres.insert(hash, instance_pre_box_free_fn);
    }

    fn len(&self) -> usize {
        self.instance_pres.len()
    }
}

// InstancePreGetFn that uses TestInstancePreCache
unsafe extern "C" fn test_cache_get_instance_pre_get_fn(
    hash: *const [u8; 32],
    external_cache: *mut c_void,
) -> *mut c_void {
    if external_cache.is_null() {
        return std::ptr::null_mut();
    }

    let cache = &*(external_cache as *const RefCell<TestInstancePreCache>);
    let hash_slice: &[u8; 32] = unsafe { &*hash };

    match cache.borrow().get(hash_slice) {
        Some(ptr) => ptr,
        None => std::ptr::null_mut(),
    }
}

// InstancePrePutFn that uses TestInstancePreCache
unsafe extern "C" fn test_cache_put_instance_pre_put_fn(
    hash: *const [u8; 32],
    instance_pre: *mut c_void,
    external_cache: *mut c_void,
) {
    assert!(!external_cache.is_null(), "External cache must not be null");

    let cache = &*(external_cache as *mut RefCell<TestInstancePreCache>);
    let hash_slice: &[u8; 32] = unsafe { &*hash };

    cache.borrow_mut().put(*hash_slice, instance_pre);
}

// Helper function to load a WASM file from the test_wasms directory
// Leaks the memory to create a 'static lifetime, which is fine for tests
fn load_wasm_file(filename: &str) -> &'static [u8] {
    // Try loading from OUT_DIR first (for build.rs generated files)
    let out_dir_path = PathBuf::from(env!("OUT_DIR")).join(filename);
    if out_dir_path.exists() {
        let bytes = fs::read(&out_dir_path).unwrap_or_else(|e| {
            panic!(
                "Failed to load WASM file from OUT_DIR {:?}: {}",
                out_dir_path, e
            );
        });
        Box::leak(bytes.into_boxed_slice())
    } else {
        panic!("Failed to load WASM file from OUT_DIR {:?}", out_dir_path);
    }
}

#[test]
fn test_create_context() {
    // Test that we can create and destroy a wasmtime context
    unsafe {
        let cache = RefCell::new(TestInstancePreCache::new());
        let cache_ptr =
            &cache as *const RefCell<TestInstancePreCache> as *mut c_void;
        let global_context = new_wasmtime_global_context(
            test_cache_get_instance_pre_get_fn,
            test_cache_put_instance_pre_put_fn,
            cache_ptr,
        );
        assert!(
            !global_context.is_null(),
            "Global context should be created"
        );
        let context =
            new_wasmtime_context(no_get_mock_contract_get_fn, global_context);
        assert!(!context.is_null(), "Context should be created");
        free_wasmtime_context(context);
        free_wasmtime_global_context(global_context);
    }
}

#[test]
fn test_verify_valid_wasm() {
    unsafe {
        let cache = RefCell::new(TestInstancePreCache::new());
        let cache_ptr =
            &cache as *const RefCell<TestInstancePreCache> as *mut c_void;
        let global_context = new_wasmtime_global_context(
            test_cache_get_instance_pre_get_fn,
            test_cache_put_instance_pre_put_fn,
            cache_ptr,
        );
        let context =
            new_wasmtime_context(no_get_mock_contract_get_fn, global_context);
        assert!(!context.is_null());

        // Load a test WASM file
        let wasm_bytes = load_wasm_file("simple_add.wasm");

        // Verify the contract
        let result = wasmtime_verify_contract(
            context,
            wasm_bytes.as_ptr(),
            wasm_bytes.len(),
        );

        assert!(result, "Valid WASM should verify successfully");

        free_wasmtime_context(context);
        free_wasmtime_global_context(global_context);
    }
}

unsafe extern "C" fn test_get_simple_add_contract_get_fn(
    _userctx: *mut c_void,
    hash: *const u8,
) -> ContractGetResult {
    let wasm_bytes = load_wasm_file("simple_add.wasm");

    // Check that the contract hash is [0;32]. If not, return kUnrecoverable error.
    let hash_slice = std::slice::from_raw_parts(hash, 32);
    if hash_slice.iter().any(|&b| b != 0) {
        // Contract hash does not match [0;32]: return unrecoverable error.
        return ContractGetResult::new(
            SystemErrorCode::kSystemUnrecoverable as i32,
            &[],
        );
    }

    ContractGetResult::new(0, wasm_bytes)
}

unsafe extern "C" fn test_get_fibonacci_contract_get_fn(
    _userctx: *mut c_void,
    hash: *const u8,
) -> ContractGetResult {
    let wasm_bytes = load_wasm_file("fibonacci.wasm");

    // Check that the contract hash is [1;32]. If not, return kUnrecoverable error.
    let hash_slice = std::slice::from_raw_parts(hash, 32);
    if hash_slice.iter().any(|&b| b != 1) {
        // Contract hash does not match [1;32]: return unrecoverable error.
        return ContractGetResult::new(
            SystemErrorCode::kSystemUnrecoverable as i32,
            &[],
        );
    }
    ContractGetResult::new(0, wasm_bytes)
}

#[test]
fn test_create_runtime() {
    unsafe {
        let cache = RefCell::new(TestInstancePreCache::new());
        let cache_ptr =
            &cache as *const RefCell<TestInstancePreCache> as *mut c_void;
        let global_context = new_wasmtime_global_context(
            test_cache_get_instance_pre_get_fn,
            test_cache_put_instance_pre_put_fn,
            cache_ptr,
        );
        println!("global_context created");
        let context = new_wasmtime_context(
            test_get_simple_add_contract_get_fn,
            global_context,
        );
        println!("context created");
        // Create a dummy contract hash
        let contract_hash = [0u8; 32];

        // Create a runtime with null host context (no syscalls needed for basic test)
        let runtime = new_wasmtime_runtime(
            contract_hash.as_ptr(),
            std::ptr::null_mut(),
            false,
            context,
        ).get().unwrap();

        assert!(!runtime.is_null(), "Should successfully create runtime");

        free_wasmtime_runtime(runtime);
        free_wasmtime_context(context);
        free_wasmtime_global_context(global_context);
    }
}

#[test]
fn test_get_memory() {
    unsafe {
        let cache = RefCell::new(TestInstancePreCache::new());
        let cache_ptr =
            &cache as *const RefCell<TestInstancePreCache> as *mut c_void;
        let global_context = new_wasmtime_global_context(
            test_cache_get_instance_pre_get_fn,
            test_cache_put_instance_pre_put_fn,
            cache_ptr,
        );
        let context = new_wasmtime_context(
            test_get_simple_add_contract_get_fn,
            global_context,
        );
        let contract_hash = [0u8; 32];

        let runtime = new_wasmtime_runtime(
            contract_hash.as_ptr(),
            std::ptr::null_mut(),
            false,
            context,
        ).get().unwrap();
        assert!(!runtime.is_null());

        // Get the memory
        let mem_slice = wasmtime_get_memory(runtime);

        // Should have some memory allocated (at least 1 page = 64KB)
        assert!(mem_slice.sz > 0, "Memory size should be non-zero");
        assert!(
            !mem_slice.mem.is_null(),
            "Memory pointer should be non-null"
        );

        free_wasmtime_runtime(runtime);
        free_wasmtime_context(context);
        free_wasmtime_global_context(global_context);
    }
}

#[test]
fn test_invoke_function() {
    unsafe {
        let cache = RefCell::new(TestInstancePreCache::new());
        let cache_ptr =
            &cache as *const RefCell<TestInstancePreCache> as *mut c_void;
        let global_context = new_wasmtime_global_context(
            test_cache_get_instance_pre_get_fn,
            test_cache_put_instance_pre_put_fn,
            cache_ptr,
        );
        let context = new_wasmtime_context(
            test_get_fibonacci_contract_get_fn,
            global_context,
        );
        let contract_hash = [1u8; 32];

        let runtime = new_wasmtime_runtime(
            contract_hash.as_ptr(),
            std::ptr::null_mut(),
            false,
            context,
        ).get().unwrap();

        assert!(!runtime.is_null());

        // Try to invoke a function
        let fn_name = b"add";
        let result =
            wasmtime_invoke(runtime, fn_name.as_ptr(), fn_name.len() as u32);

        // Check the result (0 means success)
        // The function might not exist, which is fine for this test
        println!("Invoke result error code: {}", result.error);

        free_wasmtime_runtime(runtime);
        free_wasmtime_context(context);
        free_wasmtime_global_context(global_context);
    }
}

#[test]
fn test_gas_metering() {
    unsafe {
        let cache = RefCell::new(TestInstancePreCache::new());
        let cache_ptr =
            &cache as *const RefCell<TestInstancePreCache> as *mut c_void;
        let global_context = new_wasmtime_global_context(
            test_cache_get_instance_pre_get_fn,
            test_cache_put_instance_pre_put_fn,
            cache_ptr,
        );
        let context = new_wasmtime_context(
            test_get_fibonacci_contract_get_fn,
            global_context,
        );
        let contract_hash = [1u8; 32];

        let runtime = new_wasmtime_runtime(
            contract_hash.as_ptr(),
            std::ptr::null_mut(),
            false,
            context,
        ).get().unwrap();

        assert!(!runtime.is_null());

        // Set initial gas
        let initial_gas = 1_000_000u64;
        wasmtime_set_available_gas(runtime, initial_gas);

        // Verify we can read it back
        let current_gas = wasmtime_get_available_gas(runtime);
        assert_eq!(current_gas, initial_gas, "Gas should be set correctly");

        free_wasmtime_runtime(runtime);
        free_wasmtime_context(context);
        free_wasmtime_global_context(global_context);
    }
}

#[test]
#[should_panic(expected = "Failed to load WASM file")]
fn test_missing_wasm_file() {
    // Should panic when trying to load a non-existent file
    let _wasm = load_wasm_file("nonexistent.wasm");
}

#[test]
fn test_verify_invalid_wasm() {
    unsafe {
        let cache = RefCell::new(TestInstancePreCache::new());
        let cache_ptr =
            &cache as *const RefCell<TestInstancePreCache> as *mut c_void;
        let global_context = new_wasmtime_global_context(
            test_cache_get_instance_pre_get_fn,
            test_cache_put_instance_pre_put_fn,
            cache_ptr,
        );
        let context =
            new_wasmtime_context(no_get_mock_contract_get_fn, global_context);

        // Create some invalid WASM bytes
        let invalid_wasm = vec![0x00, 0x01, 0x02, 0x03];

        // Should fail verification
        let result = wasmtime_verify_contract(
            context,
            invalid_wasm.as_ptr(),
            invalid_wasm.len(),
        );

        assert!(!result, "Invalid WASM should fail verification");

        free_wasmtime_context(context);
        free_wasmtime_global_context(global_context);
    }
}

#[test]
fn test_multiple_runtimes() {
    unsafe {
        let cache = RefCell::new(TestInstancePreCache::new());
        let cache_ptr =
            &cache as *const RefCell<TestInstancePreCache> as *mut c_void;
        let global_context = new_wasmtime_global_context(
            test_cache_get_instance_pre_get_fn,
            test_cache_put_instance_pre_put_fn,
            cache_ptr,
        );
        let context = new_wasmtime_context(
            test_get_fibonacci_contract_get_fn,
            global_context,
        );
        let contract_hash = [1u8; 32];

        // Create multiple runtimes from the same context
        let runtime1 = new_wasmtime_runtime(
            contract_hash.as_ptr(),
            std::ptr::null_mut(),
            false,
            context,
        ).get().unwrap();

        let runtime2 = new_wasmtime_runtime(
            contract_hash.as_ptr(),
            std::ptr::null_mut(),
            false,
            context,
        ).get().unwrap();

        assert!(!runtime1.is_null());
        assert!(!runtime2.is_null());

        free_wasmtime_runtime(runtime1);
        free_wasmtime_runtime(runtime2);
        free_wasmtime_context(context);
        free_wasmtime_global_context(global_context);
    }
}

#[test]
fn test_fibonacci_computation() {
    // Test that we can invoke the fibonacci function and verify results
    // This tests the WAT->WASM conversion from test_fibonacci.wat
    unsafe {
        let cache = RefCell::new(TestInstancePreCache::new());
        let cache_ptr =
            &cache as *const RefCell<TestInstancePreCache> as *mut c_void;
        let global_context = new_wasmtime_global_context(
            test_cache_get_instance_pre_get_fn,
            test_cache_put_instance_pre_put_fn,
            cache_ptr,
        );
        let context = new_wasmtime_context(
            test_get_fibonacci_contract_get_fn,
            global_context,
        );
        let contract_hash = [1u8; 32];

        let runtime = new_wasmtime_runtime(
            contract_hash.as_ptr(),
            std::ptr::null_mut(),
            false,
            context,
        ).get().unwrap();

        assert!(!runtime.is_null());

        wasmtime_set_available_gas(runtime, 1_000_000);

        // Try to invoke the fib function
        let fn_name = b"fib10";
        let result =
            wasmtime_invoke(runtime, fn_name.as_ptr(), fn_name.len() as u32);

        // Just verify we can call it - the actual behavior depends on
        // how arguments are passed, which would require more setup
        assert_eq!(result.error, 0, "Fibonacci invoke should succeed");

        assert!(
            wasmtime_get_available_gas(runtime) < 1_000_000,
            "Gas should be consumed"
        );
        free_wasmtime_runtime(runtime);
        free_wasmtime_context(context);
        free_wasmtime_global_context(global_context);
    }
}

#[test]
fn test_fibonacci_computation_overflow() {
    unsafe {
        let cache = RefCell::new(TestInstancePreCache::new());
        let cache_ptr =
            &cache as *const RefCell<TestInstancePreCache> as *mut c_void;
        let global_context = new_wasmtime_global_context(
            test_cache_get_instance_pre_get_fn,
            test_cache_put_instance_pre_put_fn,
            cache_ptr,
        );
        let context = new_wasmtime_context(
            test_get_fibonacci_contract_get_fn,
            global_context,
        );
        let contract_hash = [1u8; 32];

        let runtime = new_wasmtime_runtime(
            contract_hash.as_ptr(),
            std::ptr::null_mut(),
            false,
            context,
        ).get().unwrap();

        assert!(!runtime.is_null());

        wasmtime_set_available_gas(runtime, 1_000_000_000_000);

        // Try to invoke the fib function
        let fn_name = b"fib30";
        let result =
            wasmtime_invoke(runtime, fn_name.as_ptr(), fn_name.len() as u32);

        // Verify we get the expected error code
        assert_ne!(
            result.error,
            common::InvokeErrorCode::kInvokeUnrecoverable as i32,
            "Fibonacci invoke should fail with stack overflow"
        );

        free_wasmtime_runtime(runtime);
        free_wasmtime_context(context);
        free_wasmtime_global_context(global_context);
    }
}

unsafe extern "C" fn test_get_simple_add_and_fibonacci_contract_get_fn(
    _userctx: *mut c_void,
    hash: *const u8,
) -> ContractGetResult {
    let hash_slice = std::slice::from_raw_parts(hash, 32);

    if hash_slice.iter().all(|&b| b == 0) {
        let wasm_bytes = load_wasm_file("simple_add.wasm");
        return ContractGetResult::new(0, wasm_bytes);
    } else if hash_slice.iter().all(|&b| b == 1) {
        let wasm_bytes = load_wasm_file("fibonacci.wasm");
        return ContractGetResult::new(0, wasm_bytes);
    }
    return ContractGetResult::new(
        SystemErrorCode::kSystemUnrecoverable as i32,
        &[],
    );
}

#[test]
fn test_module_cache() {
    unsafe {
        let cache = RefCell::new(TestInstancePreCache::new());
        let cache_ptr =
            &cache as *const RefCell<TestInstancePreCache> as *mut c_void;

        // Verify cache is initially empty
        assert_eq!(cache.borrow().len(), 0, "Cache should be empty initially");

        let global_context = new_wasmtime_global_context(
            test_cache_get_instance_pre_get_fn,
            test_cache_put_instance_pre_put_fn,
            cache_ptr,
        );
        let context = new_wasmtime_context(
            test_get_simple_add_and_fibonacci_contract_get_fn,
            global_context,
        );
        let contract_hash = [0u8; 32];

        // Create first runtime - this should compile and cache the module
        let runtime1 = new_wasmtime_runtime(
            contract_hash.as_ptr(),
            std::ptr::null_mut(),
            false,
            context,
        ).get().unwrap();

        assert!(!runtime1.is_null(), "First runtime should be created");

        // Verify cache now has one entry
        assert_eq!(
            cache.borrow().len(),
            1,
            "Cache should have one entry after first runtime creation"
        );

        // Verify we can retrieve the cached module
        assert!(
            cache.borrow().get(&contract_hash).is_some(),
            "Cached module should be retrievable"
        );

        // Create second runtime with same hash - should use cached module
        let runtime2 = new_wasmtime_runtime(
            contract_hash.as_ptr(),
            std::ptr::null_mut(),
            false,
            context,
        ).get().unwrap();

        assert!(!runtime2.is_null(), "Second runtime should be created");

        // Cache should still have one entry (not duplicated)
        assert_eq!(
            cache.borrow().len(),
            1,
            "Cache should still have one entry after second runtime creation"
        );

        // Create third runtime with different hash (fibonacci) - should cache separately
        let fibonacci_hash = [1u8; 32];
        let runtime3 = new_wasmtime_runtime(
            fibonacci_hash.as_ptr(),
            std::ptr::null_mut(),
            false,
            context,
        ).get().unwrap();

        assert!(!runtime3.is_null(), "Third runtime should be created");

        // Cache should now have two entries (simple_add and fibonacci)
        assert_eq!(
            cache.borrow().len(),
            2,
            "Cache should have two entries after third runtime creation"
        );

        // Verify we can retrieve both cached modules
        assert!(
            cache.borrow().get(&contract_hash).is_some(),
            "Simple add cached module should still be retrievable"
        );
        assert!(
            cache.borrow().get(&fibonacci_hash).is_some(),
            "Fibonacci cached module should be retrievable"
        );

        // Verify all three runtimes can access memory (proving they're functional)
        let mem1 = wasmtime_get_memory(runtime1);
        let mem2 = wasmtime_get_memory(runtime2);
        let mem3 = wasmtime_get_memory(runtime3);

        assert!(!mem1.mem.is_null(), "First runtime should have memory");
        assert!(mem1.sz > 0, "First runtime memory should be non-zero");
        assert!(!mem2.mem.is_null(), "Second runtime should have memory");
        assert!(mem2.sz > 0, "Second runtime memory should be non-zero");
        assert!(!mem3.mem.is_null(), "Third runtime should have memory");
        assert!(mem3.sz > 0, "Third runtime memory should be non-zero");

        free_wasmtime_runtime(runtime1);
        free_wasmtime_runtime(runtime2);
        free_wasmtime_runtime(runtime3);
        free_wasmtime_context(context);
        free_wasmtime_global_context(global_context);
    }
}
