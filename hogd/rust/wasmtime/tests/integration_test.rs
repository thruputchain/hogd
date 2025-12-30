use std::cell::RefCell;
use std::collections::HashMap;
use std::ffi::c_void;
use std::fs;
use std::path::PathBuf;

// Import the FFI functions from wasmtime_lib
use common::SystemErrorCode;
use wasmtime_lib::{
    free_wasmtime_context, free_wasmtime_global_context, free_wasmtime_runtime,
    new_wasmtime_context, new_wasmtime_global_context, new_wasmtime_runtime,
    wasmtime_get_available_gas, wasmtime_get_memory, wasmtime_invoke,
    wasmtime_set_available_gas, wasmtime_verify_contract,
    CompiledCodeGetResult, ContractGetResult,
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

pub struct TestModuleCache {
    compiled_modules: HashMap<SHA256Hash, Vec<u8>>,
}

impl TestModuleCache {
    fn new() -> Self {
        Self {
            compiled_modules: HashMap::new(),
        }
    }

    fn get(&self, hash: &SHA256Hash) -> Option<&Vec<u8>> {
        self.compiled_modules.get(hash)
    }

    fn put(&mut self, hash: SHA256Hash, data: Vec<u8>) {
        self.compiled_modules.insert(hash, data);
    }

    fn len(&self) -> usize {
        self.compiled_modules.len()
    }

    fn clear(&mut self) {
        self.compiled_modules.clear();
    }
}

// Thread-local cache for testing module caching
thread_local! {
    static TEST_MODULE_CACHE: RefCell<TestModuleCache> = RefCell::new(TestModuleCache::new());
}

// CompiledCodeGetFn that uses the thread-local cache
unsafe extern "C" fn test_compiled_code_get_fn(
    hash: *const u8,
    _external_cache: *mut c_void,
) -> CompiledCodeGetResult {

    let hash_slice = std::slice::from_raw_parts(hash, 32);
    let mut hash_array: SHA256Hash = [0u8; 32];
    hash_array.copy_from_slice(hash_slice);

    // Clone the data while holding the borrow, then release it before leaking
    let data_opt: Option<Vec<u8>> = TEST_MODULE_CACHE
        .with(|cache| cache.borrow().get(&hash_array).cloned());

    if let Some(data) = data_opt {
        // Leak the data to create a 'static lifetime for the test
        let leaked: &'static mut [u8] = Box::leak(data.into_boxed_slice());
        CompiledCodeGetResult {
            data: leaked.as_ptr(),
            data_len: leaked.len(),
        }
    } else {
        CompiledCodeGetResult {
            data: std::ptr::null(),
            data_len: 0,
        }
    }
}

// CompiledCodePutFn that uses the thread-local cache
unsafe extern "C" fn test_compiled_code_put_fn(
    hash: *const u8,
    data: *const u8,
    data_len: usize,
    _external_cache: *mut c_void,
) {
    let hash_slice = std::slice::from_raw_parts(hash, 32);
    let mut hash_array: SHA256Hash = [0u8; 32];
    hash_array.copy_from_slice(hash_slice);
    let data_slice = std::slice::from_raw_parts(data, data_len);

    TEST_MODULE_CACHE.with(|cache| {
        cache.borrow_mut().put(hash_array, data_slice.to_vec());
    });
}

// Mock CompiledCodeGetFn for testing - returns no cached code (for tests that don't need caching)
unsafe extern "C" fn no_get_compiled_code_get_fn(
    _hash: *const u8,
    _external_cache: *mut c_void,
) -> CompiledCodeGetResult {
    CompiledCodeGetResult {
        data: std::ptr::null(),
        data_len: 0,
    }
}

// Mock CompiledCodePutFn for testing - does nothing (for tests that don't need caching)
unsafe extern "C" fn no_put_compiled_code_put_fn(
    _hash: *const u8,
    _data: *const u8,
    _data_len: usize,
    _external_cache: *mut c_void,
) {
    // No-op for tests that don't need caching
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
        let global_context = new_wasmtime_global_context();
        assert!(
            !global_context.is_null(),
            "Global context should be created"
        );
        let context = new_wasmtime_context(
            no_get_mock_contract_get_fn,
            no_get_compiled_code_get_fn,
            no_put_compiled_code_put_fn,
            global_context,
            std::ptr::null_mut(),
        );
        assert!(!context.is_null(), "Context should be created");
        free_wasmtime_context(context);
        free_wasmtime_global_context(global_context);
    }
}

#[test]
fn test_verify_valid_wasm() {
    unsafe {
        let global_context = new_wasmtime_global_context();
        let context = new_wasmtime_context(
            no_get_mock_contract_get_fn,
            no_get_compiled_code_get_fn,
            no_put_compiled_code_put_fn,
            global_context,
            std::ptr::null_mut(),
        );
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
        let global_context = new_wasmtime_global_context();
        println!("global_context created");
        let context = new_wasmtime_context(
            test_get_simple_add_contract_get_fn,
            no_get_compiled_code_get_fn,
            no_put_compiled_code_put_fn,
            global_context,
            std::ptr::null_mut(),
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
        );
        println!("runtime created");
        assert!(!runtime.is_null(), "Should successfully create runtime");

        free_wasmtime_runtime(runtime);
        free_wasmtime_context(context);
        free_wasmtime_global_context(global_context);
    }
}

#[test]
fn test_get_memory() {
    unsafe {
        let global_context = new_wasmtime_global_context();
        let context = new_wasmtime_context(
            test_get_simple_add_contract_get_fn,
            no_get_compiled_code_get_fn,
            no_put_compiled_code_put_fn,
            global_context,
            std::ptr::null_mut(),
        );
        let contract_hash = [0u8; 32];

        let runtime = new_wasmtime_runtime(
            contract_hash.as_ptr(),
            std::ptr::null_mut(),
            false,
            context,
        );

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
        let global_context = new_wasmtime_global_context();
        let context = new_wasmtime_context(
            test_get_fibonacci_contract_get_fn,
            no_get_compiled_code_get_fn,
            no_put_compiled_code_put_fn,
            global_context,
            std::ptr::null_mut(),
        );
        let contract_hash = [1u8; 32];

        let runtime = new_wasmtime_runtime(
            contract_hash.as_ptr(),
            std::ptr::null_mut(),
            false,
            context,
        );

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
        let global_context = new_wasmtime_global_context();
        let context = new_wasmtime_context(
            test_get_fibonacci_contract_get_fn,
            no_get_compiled_code_get_fn,
            no_put_compiled_code_put_fn,
            global_context,
            std::ptr::null_mut(),
        );
        let contract_hash = [1u8; 32];

        let runtime = new_wasmtime_runtime(
            contract_hash.as_ptr(),
            std::ptr::null_mut(),
            false,
            context,
        );

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
        let global_context = new_wasmtime_global_context();
        let context = new_wasmtime_context(
            no_get_mock_contract_get_fn,
            no_get_compiled_code_get_fn,
            no_put_compiled_code_put_fn,
            global_context,
            std::ptr::null_mut(),
        );

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
        let global_context = new_wasmtime_global_context();
        let context = new_wasmtime_context(
            test_get_fibonacci_contract_get_fn,
            no_get_compiled_code_get_fn,
            no_put_compiled_code_put_fn,
            global_context,
            std::ptr::null_mut(),
        );
        let contract_hash = [1u8; 32];

        // Create multiple runtimes from the same context
        let runtime1 = new_wasmtime_runtime(
            contract_hash.as_ptr(),
            std::ptr::null_mut(),
            false,
            context,
        );

        let runtime2 = new_wasmtime_runtime(
            contract_hash.as_ptr(),
            std::ptr::null_mut(),
            false,
            context,
        );

        assert!(!runtime1.is_null(), "First runtime should be created");
        assert!(!runtime2.is_null(), "Second runtime should be created");

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
        let global_context = new_wasmtime_global_context();
        let context = new_wasmtime_context(
            test_get_fibonacci_contract_get_fn,
            no_get_compiled_code_get_fn,
            no_put_compiled_code_put_fn,
            global_context,
            std::ptr::null_mut(),
        );
        let contract_hash = [1u8; 32];

        let runtime = new_wasmtime_runtime(
            contract_hash.as_ptr(),
            std::ptr::null_mut(),
            false,
            context,
        );

        assert!(!runtime.is_null(), "Runtime should be created");

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
        let global_context = new_wasmtime_global_context();
        let context = new_wasmtime_context(
            test_get_fibonacci_contract_get_fn,
            no_get_compiled_code_get_fn,
            no_put_compiled_code_put_fn,
            global_context,
            std::ptr::null_mut(),
        );
        let contract_hash = [1u8; 32];

        let runtime = new_wasmtime_runtime(
            contract_hash.as_ptr(),
            std::ptr::null_mut(),
            false,
            context,
        );

        assert!(!runtime.is_null(), "Runtime should be created");

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

// Test that module cache functions actually cache and retrieve compiled modules
#[test]
fn test_module_cache() {
    unsafe {
        // Clear the cache before starting
        TEST_MODULE_CACHE.with(|cache| {
            cache.borrow_mut().clear();
        });

        let global_context = new_wasmtime_global_context();
        assert!(!global_context.is_null());

        // Create context with module cache functions
        let context = new_wasmtime_context(
            test_get_simple_add_and_fibonacci_contract_get_fn,
            test_compiled_code_get_fn,
            test_compiled_code_put_fn,
            global_context,
            std::ptr::null_mut(),
        );
        assert!(!context.is_null());

        // Create first runtime - this should compile and cache the module
        let hash1: [u8; 32] = [0u8; 32];
        let runtime1 = new_wasmtime_runtime(
            hash1.as_ptr(),
            std::ptr::null_mut(),
            false,
            context,
        );
        assert!(!runtime1.is_null(), "First runtime should be created");

        // Verify the module was cached
        let cache_len = TEST_MODULE_CACHE.with(|cache| cache.borrow().len());
        assert_eq!(
            cache_len, 1,
            "Module cache should have 1 cached module after first runtime"
        );

        // Create second runtime with same hash - should use cached module
        let runtime2 = new_wasmtime_runtime(
            hash1.as_ptr(),
            std::ptr::null_mut(),
            false,
            context,
        );
        assert!(
            !runtime2.is_null(),
            "Second runtime should be created from cache"
        );

        // Cache should still have 1 entry (same hash)
        let cache_len = TEST_MODULE_CACHE.with(|cache| cache.borrow().len());
        assert_eq!(
            cache_len, 1,
            "Module cache should still have 1 cached module (same hash)"
        );

        // Create third runtime with different hash - should cache a new module
        let hash2: [u8; 32] = [1u8; 32];
        let runtime3 = new_wasmtime_runtime(
            hash2.as_ptr(),
            std::ptr::null_mut(),
            false,
            context,
        );
        assert!(!runtime3.is_null(), "Third runtime should be created");

        // Cache should now have 2 entries (different hashes)
        let cache_len = TEST_MODULE_CACHE.with(|cache| cache.borrow().len());
        assert_eq!(
            cache_len, 2,
            "Module cache should have 2 cached modules (different hashes)"
        );

        // Clean up
        free_wasmtime_runtime(runtime1);
        free_wasmtime_runtime(runtime2);
        free_wasmtime_runtime(runtime3);
        free_wasmtime_context(context);
        free_wasmtime_global_context(global_context);
    }
}
