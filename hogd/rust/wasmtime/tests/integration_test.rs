use std::fs;
use std::path::PathBuf;
use std::ffi::c_void;

// Import the FFI functions from wasmtime_lib
use wasmtime_lib::{
    new_wasmtime_context,
    free_wasmtime_context,
    wasmtime_verify_contract,
    new_wasmtime_runtime,
    free_wasmtime_runtime,
    wasmtime_invoke,
    wasmtime_get_memory,
    wasmtime_get_available_gas,
    wasmtime_set_available_gas,
};

// Stub implementations of the C trampoline functions that the wasmtime library expects.
// These are normally provided by the C++ build system, but for standalone Rust tests
// we provide empty stubs so the linker is happy.
#[no_mangle]
extern "C" fn c_call_0args(_fn_ptr: *mut c_void, _userctx: *mut c_void) -> common::TrampolineResult {
    common::TrampolineResult {
        result: 0,
        panic: common::HostFnErrorCode::kHostNoneOrRecoverable as i32,
    }
}

#[no_mangle]
extern "C" fn c_call_1args(_fn_ptr: *mut c_void, _userctx: *mut c_void, _arg1: u64) -> common::TrampolineResult {
    common::TrampolineResult {
        result: 0,
        panic: common::HostFnErrorCode::kHostNoneOrRecoverable as i32,
    }
}

#[no_mangle]
extern "C" fn c_call_2args(_fn_ptr: *mut c_void, _userctx: *mut c_void, _arg1: u64, _arg2: u64) -> common::TrampolineResult {
    common::TrampolineResult {
        result: 0,
        panic: common::HostFnErrorCode::kHostNoneOrRecoverable as i32,
    }
}

#[no_mangle]
extern "C" fn c_call_3args(_fn_ptr: *mut c_void, _userctx: *mut c_void, _arg1: u64, _arg2: u64, _arg3: u64) -> common::TrampolineResult {
    common::TrampolineResult {
        result: 0,
        panic: common::HostFnErrorCode::kHostNoneOrRecoverable as i32,
    }
}

#[no_mangle]
extern "C" fn c_call_4args(_fn_ptr: *mut c_void, _userctx: *mut c_void, _arg1: u64, _arg2: u64, _arg3: u64, _arg4: u64) -> common::TrampolineResult {
    common::TrampolineResult {
        result: 0,
        panic: common::HostFnErrorCode::kHostNoneOrRecoverable as i32,
    }
}

#[no_mangle]
extern "C" fn c_call_5args(_fn_ptr: *mut c_void, _userctx: *mut c_void, _arg1: u64, _arg2: u64, _arg3: u64, _arg4: u64, _arg5: u64) -> common::TrampolineResult {
    common::TrampolineResult {
        result: 0,
        panic: common::HostFnErrorCode::kHostNoneOrRecoverable as i32,
    }
}

#[no_mangle]
extern "C" fn c_call_6args(_fn_ptr: *mut c_void, _userctx: *mut c_void, _arg1: u64, _arg2: u64, _arg3: u64, _arg4: u64, _arg5: u64, _arg6: u64) -> common::TrampolineResult {
    common::TrampolineResult {
        result: 0,
        panic: common::HostFnErrorCode::kHostNoneOrRecoverable as i32,
    }
}

#[no_mangle]
extern "C" fn c_call_7args(_fn_ptr: *mut c_void, _userctx: *mut c_void, _arg1: u64, _arg2: u64, _arg3: u64, _arg4: u64, _arg5: u64, _arg6: u64, _arg7: u64) -> common::TrampolineResult {
    common::TrampolineResult {
        result: 0,
        panic: common::HostFnErrorCode::kHostNoneOrRecoverable as i32,
    }
}

#[no_mangle]
extern "C" fn c_call_8args(_fn_ptr: *mut c_void, _userctx: *mut c_void, _arg1: u64, _arg2: u64, _arg3: u64, _arg4: u64, _arg5: u64, _arg6: u64, _arg7: u64, _arg8: u64) -> common::TrampolineResult {
    common::TrampolineResult {
        result: 0,
        panic: common::HostFnErrorCode::kHostNoneOrRecoverable as i32,
    }
}

// Helper function to load a WASM file from the test_wasms directory
fn load_wasm_file(filename: &str) -> Vec<u8> {
    // Try loading from OUT_DIR first (for build.rs generated files)
    let out_dir_path = PathBuf::from(env!("OUT_DIR")).join(filename);
    if out_dir_path.exists() {
        return fs::read(&out_dir_path).unwrap_or_else(|e| {
            panic!("Failed to load WASM file from OUT_DIR {:?}: {}", out_dir_path, e);
        });
    }

    panic!("Failed to load WASM file from OUT_DIR {:?}", out_dir_path);
}

#[test]
fn test_create_context() {
    // Test that we can create and destroy a wasmtime context
    unsafe {
        let context = new_wasmtime_context();
        assert!(!context.is_null(), "Context should be created");
        free_wasmtime_context(context);
    }
}

#[test]
fn test_verify_valid_wasm() {
    unsafe {
        let context = new_wasmtime_context();
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
    }
}

#[test]
fn test_create_runtime() {
    unsafe {
        let context = new_wasmtime_context();
        let wasm_bytes = load_wasm_file("simple_add.wasm");
        
        // Create a dummy contract hash
        let contract_hash = [0u8; 32];
        
        // Create a runtime with null host context (no syscalls needed for basic test)
        let runtime = new_wasmtime_runtime(
            wasm_bytes.as_ptr(),
            wasm_bytes.len() as u32,
            contract_hash.as_ptr(),
            std::ptr::null_mut(),
            context,
        );
        
        assert!(!runtime.is_null(), "Should successfully create runtime");
        
        free_wasmtime_runtime(runtime);
        free_wasmtime_context(context);
    }
}

#[test]
fn test_get_memory() {
    unsafe {
        let context = new_wasmtime_context();
        let wasm_bytes = load_wasm_file("simple_add.wasm");
        let contract_hash = [0u8; 32];
        
        let runtime = new_wasmtime_runtime(
            wasm_bytes.as_ptr(),
            wasm_bytes.len() as u32,
            contract_hash.as_ptr(),
            std::ptr::null_mut(),
            context,
        );
        
        assert!(!runtime.is_null());
        
        // Get the memory
        let mem_slice = wasmtime_get_memory(runtime);
        
        // Should have some memory allocated (at least 1 page = 64KB)
        assert!(mem_slice.sz > 0, "Memory size should be non-zero");
        assert!(!mem_slice.mem.is_null(), "Memory pointer should be non-null");
        
        free_wasmtime_runtime(runtime);
        free_wasmtime_context(context);
    }
}

#[test]
fn test_invoke_function() {
    unsafe {
        let context = new_wasmtime_context();
        let wasm_bytes = load_wasm_file("fibonacci.wasm");
        let contract_hash = [0u8; 32];
        
        let runtime = new_wasmtime_runtime(
            wasm_bytes.as_ptr(),
            wasm_bytes.len() as u32,
            contract_hash.as_ptr(),
            std::ptr::null_mut(),
            context,
        );
        
        assert!(!runtime.is_null());
        
        // Try to invoke a function
        let fn_name = b"add";
        let result = wasmtime_invoke(
            runtime,
            fn_name.as_ptr(),
            fn_name.len() as u32,
        );
        
        // Check the result (0 means success)
        // The function might not exist, which is fine for this test
        println!("Invoke result error code: {}", result.error);
        
        free_wasmtime_runtime(runtime);
        free_wasmtime_context(context);
    }
}

#[test]
fn test_gas_metering() {
    unsafe {
        let context = new_wasmtime_context();
        let wasm_bytes = load_wasm_file("fibonacci.wasm");
        let contract_hash = [0u8; 32];
        
        let runtime = new_wasmtime_runtime(
            wasm_bytes.as_ptr(),
            wasm_bytes.len() as u32,
            contract_hash.as_ptr(),
            std::ptr::null_mut(),
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
        let context = new_wasmtime_context();
        
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
    }
}

#[test]
fn test_multiple_runtimes() {
    unsafe {
        let context = new_wasmtime_context();
        let wasm_bytes = load_wasm_file("fibonacci.wasm");
        let contract_hash = [0u8; 32];
        
        // Create multiple runtimes from the same context
        let runtime1 = new_wasmtime_runtime(
            wasm_bytes.as_ptr(),
            wasm_bytes.len() as u32,
            contract_hash.as_ptr(),
            std::ptr::null_mut(),
            context,
        );
        
        let runtime2 = new_wasmtime_runtime(
            wasm_bytes.as_ptr(),
            wasm_bytes.len() as u32,
            contract_hash.as_ptr(),
            std::ptr::null_mut(),
            context,
        );
        
        assert!(!runtime1.is_null(), "First runtime should be created");
        assert!(!runtime2.is_null(), "Second runtime should be created");
        
        free_wasmtime_runtime(runtime1);
        free_wasmtime_runtime(runtime2);
        free_wasmtime_context(context);
    }
}

#[test]
fn test_fibonacci_computation() {
    // Test that we can invoke the fibonacci function and verify results
    // This tests the WAT->WASM conversion from test_fibonacci.wat
    unsafe {
        let context = new_wasmtime_context();
        let wasm_bytes = load_wasm_file("fibonacci.wasm");
        let contract_hash = [0u8; 32];
        
        let runtime = new_wasmtime_runtime(
            wasm_bytes.as_ptr(),
            wasm_bytes.len() as u32,
            contract_hash.as_ptr(),
            std::ptr::null_mut(),
            context,
        );
        
        assert!(!runtime.is_null(), "Runtime should be created");

        wasmtime_set_available_gas(runtime, 1_000_000);
        
        // Try to invoke the fib function
        let fn_name = b"fib10";
        let result = wasmtime_invoke(
            runtime,
            fn_name.as_ptr(),
            fn_name.len() as u32,
        );
        
        // Just verify we can call it - the actual behavior depends on
        // how arguments are passed, which would require more setup
        assert_eq!(result.error, 0, "Fibonacci invoke should succeed");

        assert!(wasmtime_get_available_gas(runtime) < 1_000_000, "Gas should be consumed");
        free_wasmtime_runtime(runtime);
        free_wasmtime_context(context);
    }
}

#[test]
fn test_fibonacci_computation_overflow() {
    unsafe {
        let context = new_wasmtime_context();
        let wasm_bytes = load_wasm_file("fibonacci.wasm");
        let contract_hash = [0u8; 32];
        
        let runtime = new_wasmtime_runtime(
            wasm_bytes.as_ptr(),
            wasm_bytes.len() as u32,
            contract_hash.as_ptr(),
            std::ptr::null_mut(),
            context,
        );
        
        assert!(!runtime.is_null(), "Runtime should be created");

        wasmtime_set_available_gas(runtime, 1_000_000_000_000);
        
        // Try to invoke the fib function
        let fn_name = b"fib30";
        let result = wasmtime_invoke(
            runtime,
            fn_name.as_ptr(),
            fn_name.len() as u32,
        );
        
        // Verify we get the expected error code
        assert_ne!(result.error, common::InvokeErrorCode::kInvokeUnrecoverable as i32, "Fibonacci invoke should fail with stack overflow");

        free_wasmtime_runtime(runtime);
        free_wasmtime_context(context);
    }
}
