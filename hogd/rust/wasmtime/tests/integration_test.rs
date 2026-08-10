use std::ffi::c_void;
use std::fs;
use std::path::PathBuf;

// Import the FFI functions from wasmtime_lib
use common::SystemErrorCode;
use wasmtime_lib::{
    free_wasmtime_context, free_wasmtime_global_context, free_wasmtime_runtime,
    new_wasmtime_context, new_wasmtime_global_context, new_wasmtime_runtime,
    wasmtime_invoke, wasmtime_verify_contract, ContractGetResult,
};

// Stub implementations of the C trampoline functions that the wasmtime library expects.
// These are normally provided by the C++ build system, but for standalone Rust tests
// we provide empty stubs so the linker is happy.
#[no_mangle]
extern "C" fn c_call_0args(
    _fn_ptr: *mut c_void,
    _userctx: *mut c_void,
    _wasm_context: common::WasmContext,
) -> common::TrampolineResult {
    common::TrampolineResult {
        result_code: 0,
        panic: common::HostFnErrorCode::NoneOrRecoverable.0,
        gas_consumed: 0,
    }
}

#[no_mangle]
extern "C" fn c_call_1args(
    _fn_ptr: *mut c_void,
    _userctx: *mut c_void,
    _wasm_context: common::WasmContext,
    _arg1: u64,
) -> common::TrampolineResult {
    common::TrampolineResult {
        result_code: 0,
        panic: common::HostFnErrorCode::NoneOrRecoverable.0,
        gas_consumed: 0,
    }
}

#[no_mangle]
extern "C" fn c_call_2args(
    _fn_ptr: *mut c_void,
    _userctx: *mut c_void,
    _wasm_context: common::WasmContext,
    _arg1: u64,
    _arg2: u64,
) -> common::TrampolineResult {
    common::TrampolineResult {
        result_code: 0,
        panic: common::HostFnErrorCode::NoneOrRecoverable.0,
        gas_consumed: 0,
    }
}

#[no_mangle]
extern "C" fn c_call_3args(
    _fn_ptr: *mut c_void,
    _userctx: *mut c_void,
    _wasm_context: common::WasmContext,
    _arg1: u64,
    _arg2: u64,
    _arg3: u64,
) -> common::TrampolineResult {
    common::TrampolineResult {
        result_code: 0,
        panic: common::HostFnErrorCode::NoneOrRecoverable.0,
        gas_consumed: 0,
    }
}

#[no_mangle]
extern "C" fn c_call_4args(
    _fn_ptr: *mut c_void,
    _userctx: *mut c_void,
    _wasm_context: common::WasmContext,
    _arg1: u64,
    _arg2: u64,
    _arg3: u64,
    _arg4: u64,
) -> common::TrampolineResult {
    common::TrampolineResult {
        result_code: 0,
        panic: common::HostFnErrorCode::NoneOrRecoverable.0,
        gas_consumed: 0,
    }
}

#[no_mangle]
extern "C" fn c_call_5args(
    _fn_ptr: *mut c_void,
    _userctx: *mut c_void,
    _wasm_context: common::WasmContext,
    _arg1: u64,
    _arg2: u64,
    _arg3: u64,
    _arg4: u64,
    _arg5: u64,
) -> common::TrampolineResult {
    common::TrampolineResult {
        result_code: 0,
        panic: common::HostFnErrorCode::NoneOrRecoverable.0,
        gas_consumed: 0,
    }
}

#[no_mangle]
extern "C" fn c_call_6args(
    _fn_ptr: *mut c_void,
    _userctx: *mut c_void,
    _wasm_context: common::WasmContext,
    _arg1: u64,
    _arg2: u64,
    _arg3: u64,
    _arg4: u64,
    _arg5: u64,
    _arg6: u64,
) -> common::TrampolineResult {
    common::TrampolineResult {
        result_code: 0,
        panic: common::HostFnErrorCode::NoneOrRecoverable.0,
        gas_consumed: 0,
    }
}

#[no_mangle]
extern "C" fn c_call_7args(
    _fn_ptr: *mut c_void,
    _userctx: *mut c_void,
    _wasm_context: common::WasmContext,
    _arg1: u64,
    _arg2: u64,
    _arg3: u64,
    _arg4: u64,
    _arg5: u64,
    _arg6: u64,
    _arg7: u64,
) -> common::TrampolineResult {
    common::TrampolineResult {
        result_code: 0,
        panic: common::HostFnErrorCode::NoneOrRecoverable.0,
        gas_consumed: 0,
    }
}

#[no_mangle]
extern "C" fn c_call_8args(
    _fn_ptr: *mut c_void,
    _userctx: *mut c_void,
    _wasm_context: common::WasmContext,
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
        result_code: 0,
        panic: common::HostFnErrorCode::NoneOrRecoverable.0,
        gas_consumed: 0,
    }
}

// Mock ContractGetFn for testing
// Returns the WASM bytes stored in MOCK_CONTRACT_DATA
unsafe extern "C" fn no_get_mock_contract_get_fn(
    _userctx: *mut c_void,
    _hash: *const u8,
) -> ContractGetResult {
    ContractGetResult::new(SystemErrorCode::Unrecoverable.0, &[])
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
        let global_context = new_wasmtime_global_context(8);
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
        let global_context = new_wasmtime_global_context(8);
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

        assert_eq!(result, 0, "Valid WASM should verify successfully");

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
        return ContractGetResult::new(SystemErrorCode::Unrecoverable.0, &[]);
    }

    ContractGetResult::new(0, wasm_bytes)
}

unsafe extern "C" fn test_get_fibonacci_contract_get_fn(
    userctx: *mut c_void,
    hash: *const u8,
) -> ContractGetResult {
    assert!(!userctx.is_null());
    let wasm_bytes = load_wasm_file("fibonacci.wasm");

    // Check that the contract hash is [1;32]. If not, return kUnrecoverable error.
    let hash_slice = std::slice::from_raw_parts(hash, 32);
    if hash_slice.iter().any(|&b| b != 1) {
        // Contract hash does not match [1;32]: return unrecoverable error.
        return ContractGetResult::new(SystemErrorCode::Unrecoverable.0, &[]);
    }
    ContractGetResult::new(0, wasm_bytes)
}

#[test]
fn test_create_runtime() {
    unsafe {
        let global_context = new_wasmtime_global_context(8);
        let context = new_wasmtime_context(
            test_get_simple_add_contract_get_fn,
            global_context,
        );
        // Create a dummy contract hash
        let contract_hash = [0u8; 32];

        let context_ptr: *mut c_void =
            core::mem::transmute(Box::into_raw(Box::new(1)));

        // Create a runtime with null host context (no syscalls needed for basic test)
        let runtime =
            new_wasmtime_runtime(contract_hash.as_ptr(), context_ptr, context)
                .get()
                .unwrap();

        assert!(!runtime.is_null(), "Should successfully create runtime");

        free_wasmtime_runtime(runtime);
        free_wasmtime_context(context);
        free_wasmtime_global_context(global_context);

        let _: Box<i32> = Box::from_raw(core::mem::transmute(context_ptr));
    }
}

#[test]
fn test_invoke_function() {
    unsafe {
        let global_context = new_wasmtime_global_context(8);
        let context = new_wasmtime_context(
            test_get_fibonacci_contract_get_fn,
            global_context,
        );
        let contract_hash = [1u8; 32];

        let context_ptr: *mut c_void =
            core::mem::transmute(Box::into_raw(Box::new(1)));

        let runtime =
            new_wasmtime_runtime(contract_hash.as_ptr(), context_ptr, context)
                .get()
                .unwrap();

        assert!(!runtime.is_null());

        // Try to invoke a function
        let fn_name = b"add";
        let result = wasmtime_invoke(
            runtime,
            fn_name.as_ptr(),
            fn_name.len() as u32,
            1_000_000,
        );

        // Check the result (0 means success)
        // The function might not exist, which is fine for this test
        println!("Invoke result error code: {}", result.error);

        free_wasmtime_runtime(runtime);
        free_wasmtime_context(context);
        free_wasmtime_global_context(global_context);

        let _: Box<i32> = Box::from_raw(core::mem::transmute(context_ptr));
    }
}

#[test]
fn test_gas_metering() {
    unsafe {
        let global_context = new_wasmtime_global_context(8);
        let context = new_wasmtime_context(
            test_get_fibonacci_contract_get_fn,
            global_context,
        );
        let contract_hash = [1u8; 32];

        let context_ptr: *mut c_void =
            core::mem::transmute(Box::into_raw(Box::new(1)));

        let runtime =
            new_wasmtime_runtime(contract_hash.as_ptr(), context_ptr, context)
                .get()
                .unwrap();

        assert!(!runtime.is_null());

        // Invoke with a gas limit and verify gas was consumed
        let fn_name = b"fib10";
        let gas_limit = 1_000_000u64;
        let result = wasmtime_invoke(
            runtime,
            fn_name.as_ptr(),
            fn_name.len() as u32,
            gas_limit,
        );

        assert_eq!(result.error, 0, "Fibonacci invoke should succeed");
        assert!(result.gas_consumed > 0, "Some gas should be consumed");
        assert!(
            result.gas_consumed <= gas_limit,
            "Gas consumed should not exceed limit"
        );

        free_wasmtime_runtime(runtime);
        free_wasmtime_context(context);
        free_wasmtime_global_context(global_context);

        let _: Box<i32> = Box::from_raw(core::mem::transmute(context_ptr));
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
        let global_context = new_wasmtime_global_context(8);
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

        assert_ne!(result, 0, "Invalid WASM should fail verification");

        free_wasmtime_context(context);
        free_wasmtime_global_context(global_context);
    }
}

#[test]
fn test_multiple_runtimes() {
    unsafe {
        let global_context = new_wasmtime_global_context(8);
        let context = new_wasmtime_context(
            test_get_fibonacci_contract_get_fn,
            global_context,
        );
        let contract_hash = [1u8; 32];

        // Create multiple runtimes from the same context
        let context_ptr: *mut c_void =
            core::mem::transmute(Box::into_raw(Box::new(1)));

        let runtime1 =
            new_wasmtime_runtime(contract_hash.as_ptr(), context_ptr, context)
                .get()
                .unwrap();

        let runtime2 =
            new_wasmtime_runtime(contract_hash.as_ptr(), context_ptr, context)
                .get()
                .unwrap();

        assert!(!runtime1.is_null());
        assert!(!runtime2.is_null());

        free_wasmtime_runtime(runtime1);
        free_wasmtime_runtime(runtime2);
        free_wasmtime_context(context);
        free_wasmtime_global_context(global_context);

        let _: Box<i32> = Box::from_raw(core::mem::transmute(context_ptr));
    }
}

#[test]
fn test_fibonacci_computation() {
    unsafe {
        let global_context = new_wasmtime_global_context(8);
        let context = new_wasmtime_context(
            test_get_fibonacci_contract_get_fn,
            global_context,
        );
        let contract_hash = [1u8; 32];

        let context_ptr: *mut c_void =
            core::mem::transmute(Box::into_raw(Box::new(1)));

        let runtime =
            new_wasmtime_runtime(contract_hash.as_ptr(), context_ptr, context)
                .get()
                .unwrap();

        assert!(!runtime.is_null());

        // Try to invoke the fib function
        let fn_name = b"fib10";
        let result = wasmtime_invoke(
            runtime,
            fn_name.as_ptr(),
            fn_name.len() as u32,
            1_000_000,
        );

        assert_eq!(result.error, 0, "Fibonacci invoke should succeed");

        assert!(result.gas_consumed > 0, "Gas should be consumed");
        free_wasmtime_runtime(runtime);
        free_wasmtime_context(context);
        free_wasmtime_global_context(global_context);

        let _: Box<i32> = Box::from_raw(core::mem::transmute(context_ptr));
    }
}

#[test]
fn test_fibonacci_computation_overflow() {
    unsafe {
        let global_context = new_wasmtime_global_context(8);
        let context = new_wasmtime_context(
            test_get_fibonacci_contract_get_fn,
            global_context,
        );
        let contract_hash = [1u8; 32];

        let context_ptr: *mut c_void =
            core::mem::transmute(Box::into_raw(Box::new(1)));

        let runtime =
            new_wasmtime_runtime(contract_hash.as_ptr(), context_ptr, context)
                .get()
                .unwrap();

        assert!(!runtime.is_null());

        // Try to invoke the fib function
        let fn_name = b"fib30";
        let result = wasmtime_invoke(
            runtime,
            fn_name.as_ptr(),
            fn_name.len() as u32,
            1_000_000_000_000,
        );

        // Verify we get the expected error code
        assert_ne!(
            result.error,
            common::InvokeErrorCode::Unrecoverable.0,
            "Fibonacci invoke should fail with stack overflow"
        );

        free_wasmtime_runtime(runtime);
        free_wasmtime_context(context);
        free_wasmtime_global_context(global_context);

        let _: Box<i32> = Box::from_raw(core::mem::transmute(context_ptr));
    }
}

unsafe extern "C" fn test_get_oob_data_segment_contract_get_fn(
    _userctx: *mut c_void,
    hash: *const u8,
) -> ContractGetResult {
    let wasm_bytes = load_wasm_file("oob_data_segment.wasm");

    let hash_slice = std::slice::from_raw_parts(hash, 32);
    if hash_slice.iter().any(|&b| b != 2) {
        return ContractGetResult::new(SystemErrorCode::Unrecoverable.0, &[]);
    }
    ContractGetResult::new(0, wasm_bytes)
}

unsafe extern "C" fn test_get_param_entrypoint_contract_get_fn(
    _userctx: *mut c_void,
    hash: *const u8,
) -> ContractGetResult {
    let wasm_bytes = load_wasm_file("param_entrypoint.wasm");

    let hash_slice = std::slice::from_raw_parts(hash, 32);
    if hash_slice.iter().any(|&b| b != 4) {
        return ContractGetResult::new(SystemErrorCode::Unrecoverable.0, &[]);
    }

    ContractGetResult::new(0, wasm_bytes)
}

unsafe extern "C" fn test_get_oob_element_segment_contract_get_fn(
    _userctx: *mut c_void,
    hash: *const u8,
) -> ContractGetResult {
    let wasm_bytes = load_wasm_file("oob_element_segment.wasm");

    let hash_slice = std::slice::from_raw_parts(hash, 32);
    if hash_slice.iter().any(|&b| b != 3) {
        return ContractGetResult::new(SystemErrorCode::Unrecoverable.0, &[]);
    }
    ContractGetResult::new(0, wasm_bytes)
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
    return ContractGetResult::new(SystemErrorCode::Unrecoverable.0, &[]);
}

#[test]
fn test_module_cache() {
    unsafe {
        let global_context = new_wasmtime_global_context(8);
        let context = new_wasmtime_context(
            test_get_simple_add_and_fibonacci_contract_get_fn,
            global_context,
        );
        let contract_hash = [0u8; 32];

        let context_ptr: *mut c_void =
            core::mem::transmute(Box::into_raw(Box::new(1)));

        // Create first runtime - this should compile and cache the module
        let runtime1 =
            new_wasmtime_runtime(contract_hash.as_ptr(), context_ptr, context)
                .get()
                .unwrap();

        assert!(!runtime1.is_null(), "First runtime should be created");

        // Create second runtime with same hash - should use cached module
        let runtime2 =
            new_wasmtime_runtime(contract_hash.as_ptr(), context_ptr, context)
                .get()
                .unwrap();

        assert!(!runtime2.is_null(), "Second runtime should be created");

        // Create third runtime with different hash (fibonacci) - should cache separately
        let fibonacci_hash = [1u8; 32];
        let runtime3 =
            new_wasmtime_runtime(fibonacci_hash.as_ptr(), context_ptr, context)
                .get()
                .unwrap();

        assert!(!runtime3.is_null(), "Third runtime should be created");

        free_wasmtime_runtime(runtime1);
        free_wasmtime_runtime(runtime2);
        free_wasmtime_runtime(runtime3);
        free_wasmtime_context(context);
        free_wasmtime_global_context(global_context);

        let _: Box<i32> = Box::from_raw(core::mem::transmute(context_ptr));
    }
}

#[test]
fn test_audit_1_oob_data_segment_instantiation() {
    unsafe {
        let global_context = new_wasmtime_global_context(8);
        let context = new_wasmtime_context(
            test_get_oob_data_segment_contract_get_fn,
            global_context,
        );
        let contract_hash = [2u8; 32];

        let context_ptr: *mut c_void =
            core::mem::transmute(Box::into_raw(Box::new(1)));

        let result =
            new_wasmtime_runtime(contract_hash.as_ptr(), context_ptr, context);

        let runtime_ptr = result.get().unwrap();
        assert!(
            runtime_ptr.is_null(),
            "Should return null for OOB data segment module, not panic"
        );

        free_wasmtime_context(context);
        free_wasmtime_global_context(global_context);

        let _: Box<i32> = Box::from_raw(core::mem::transmute(context_ptr));
    }
}

#[test]
fn test_audit_1_oob_element_segment_instantiation() {
    unsafe {
        let global_context = new_wasmtime_global_context(8);
        let context = new_wasmtime_context(
            test_get_oob_element_segment_contract_get_fn,
            global_context,
        );
        let contract_hash = [3u8; 32];

        let context_ptr: *mut c_void =
            core::mem::transmute(Box::into_raw(Box::new(1)));

        let result =
            new_wasmtime_runtime(contract_hash.as_ptr(), context_ptr, context);

        let runtime_ptr = result.get().unwrap();
        assert!(
            runtime_ptr.is_null(),
            "Should return null for OOB element segment module, not panic"
        );

        free_wasmtime_context(context);
        free_wasmtime_global_context(global_context);

        let _: Box<i32> = Box::from_raw(core::mem::transmute(context_ptr));
    }
}

#[test]
fn test_pool_exhaustion_returns_system_error() {
    unsafe {
        let global_context = new_wasmtime_global_context(1);
        let context = new_wasmtime_context(
            test_get_simple_add_contract_get_fn,
            global_context,
        );
        let contract_hash = [0u8; 32];
        let context_ptr: *mut c_void =
            core::mem::transmute(Box::into_raw(Box::new(1)));

        let mut runtimes: Vec<*mut c_void> = Vec::with_capacity(1024);
        for _ in 0..1024 {
            let result = new_wasmtime_runtime(
                contract_hash.as_ptr(),
                context_ptr,
                context,
            );
            let runtime_ptr = result
                .get()
                .expect("Expected success before pool capacity is exhausted");
            assert!(
                !runtime_ptr.is_null(),
                "Expected non-null runtime before pool exhaustion"
            );
            runtimes.push(runtime_ptr);
        }

        let exhausted_result =
            new_wasmtime_runtime(contract_hash.as_ptr(), context_ptr, context);
        assert!(
            exhausted_result.get().is_none(),
            "Expected err_code != 0 on pool exhaustion, not panic"
        );

        for runtime_ptr in runtimes {
            free_wasmtime_runtime(runtime_ptr);
        }
        free_wasmtime_context(context);
        free_wasmtime_global_context(global_context);

        let _: Box<i32> = Box::from_raw(core::mem::transmute(context_ptr));
    }
}

#[test]
fn test_param_entrypoint_signature_mismatch_is_deterministic() {
    unsafe {
        let global_context = new_wasmtime_global_context(8);
        let context = new_wasmtime_context(
            test_get_param_entrypoint_contract_get_fn,
            global_context,
        );
        let contract_hash = [4u8; 32];
        let context_ptr: *mut c_void =
            core::mem::transmute(Box::into_raw(Box::new(1)));

        let wasm_bytes = load_wasm_file("param_entrypoint.wasm");
        let verified = wasmtime_verify_contract(
            context,
            wasm_bytes.as_ptr(),
            wasm_bytes.len(),
        );
        assert_eq!(
            verified, 0,
            "verify_contract currently accepts a method export with the wrong ABI"
        );

        let runtime =
            new_wasmtime_runtime(contract_hash.as_ptr(), context_ptr, context)
                .get()
                .unwrap();
        assert!(
            !runtime.is_null(),
            "runtime creation should not reject the bad entrypoint ABI yet"
        );

        let fn_name = b"0000000000000001";
        let result = wasmtime_invoke(
            runtime,
            fn_name.as_ptr(),
            fn_name.len() as u32,
            1_000_000,
        );

        assert_eq!(
            result.error,
            common::InvokeErrorCode::Deterministic.0,
            "invoke-time signature mismatch should be classified as deterministic"
        );

        free_wasmtime_runtime(runtime);
        free_wasmtime_context(context);
        free_wasmtime_global_context(global_context);

        let _: Box<i32> = Box::from_raw(core::mem::transmute(context_ptr));
    }
}

fn big_stack_frame_contract_bytes() -> &'static [u8] {
    let n = 128;
    let mut wat = String::from("(module\n  (func $big_stack\n");
    for _ in 0..n {
        wat.push_str("    call $get\n");
    }
    for _ in 0..n {
        wat.push_str("    call $take\n");
    }
    wat.push_str("  )\n");
    wat.push_str(
        "  (func $get (result i64 i64 i64 i64 i64 i64 i64 i64 i64 i64)\n\
         \x20   call $big_stack\n    unreachable)\n",
    );
    wat.push_str(
        "  (func $take (param i64 i64 i64 i64 i64 i64 i64 i64 i64 i64)\n\
         \x20   unreachable)\n",
    );
    wat.push_str(
        "  (func (export \"0000000000000001\")\n    call $big_stack)\n\
         \x20 (memory 1 1)\n  (export \"memory\" (memory 0))\n)\n",
    );
    let bytes = wat::parse_str(&wat).expect("big stack frame WAT must parse");
    Box::leak(bytes.into_boxed_slice())
}

#[test]
fn test_issue_1869_big_stack_frame_rejected_by_verify() {
    unsafe {
        let global_context = new_wasmtime_global_context(8);
        let context =
            new_wasmtime_context(no_get_mock_contract_get_fn, global_context);

        let wasm_bytes = big_stack_frame_contract_bytes();
        let result = wasmtime_verify_contract(
            context,
            wasm_bytes.as_ptr(),
            wasm_bytes.len(),
        );

        assert_ne!(
            result, 0,
            "contracts with operand stack exceeding wasm-meter limits \
             must be rejected by wasmtime_verify_contract"
        );

        free_wasmtime_context(context);
        free_wasmtime_global_context(global_context);
    }
}

unsafe extern "C" fn test_get_simd_contract_get_fn(
    _userctx: *mut c_void,
    hash: *const u8,
) -> ContractGetResult {
    let wasm_bytes = load_wasm_file("simd_v128.wasm");

    let hash_slice = std::slice::from_raw_parts(hash, 32);
    if hash_slice.iter().any(|&b| b != 4) {
        return ContractGetResult::new(SystemErrorCode::Unrecoverable.0, &[]);
    }
    ContractGetResult::new(0, wasm_bytes)
}

#[test]
fn test_h002_production_config_rejects_simd_at_validation() {
    let config =
        wasmtime_lib::global_context::WasmtimeGlobalContext::new_config();
    let engine = wasmtime::Engine::new(&config)
        .expect("production config should produce a valid host engine");
    let simd_bytes = load_wasm_file("simd_v128.wasm");

    let err = wasmtime::Module::new(&engine, simd_bytes).err().expect(
        "production config must disable SIMD so v128 modules are rejected \
         at validation on every host",
    );

    let msg = format!("{err:#}");
    assert!(
        msg.contains("SIMD support is not enabled"),
        "expected wasmparser validation rejection \"SIMD support is not \
         enabled\", got host-dependent codegen error instead: {err:#}"
    );
}

#[test]
fn test_h002_verify_contract_rejects_simd_module() {
    unsafe {
        let global_context = new_wasmtime_global_context(8);
        let context =
            new_wasmtime_context(no_get_mock_contract_get_fn, global_context);

        let wasm_bytes = load_wasm_file("simd_v128.wasm");
        let result = wasmtime_verify_contract(
            context,
            wasm_bytes.as_ptr(),
            wasm_bytes.len(),
        );

        assert_ne!(
            result, 0,
            "SIMD-bearing contracts must be rejected by \
             wasmtime_verify_contract on every host"
        );

        free_wasmtime_context(context);
        free_wasmtime_global_context(global_context);
    }
}

#[test]
fn test_h002_runtime_creation_on_simd_returns_system_unrecoverable() {
    unsafe {
        let global_context = new_wasmtime_global_context(8);
        let context =
            new_wasmtime_context(test_get_simd_contract_get_fn, global_context);
        let contract_hash = [4u8; 32];

        let context_ptr: *mut c_void =
            core::mem::transmute(Box::into_raw(Box::new(1)));

        let result =
            new_wasmtime_runtime(contract_hash.as_ptr(), context_ptr, context);

        assert!(
            result.get().is_none(),
            "SIMD contract must not yield a runtime"
        );
        assert_eq!(
            result.err_code(),
            SystemErrorCode::Unrecoverable.0,
            "Module::new failure on SIMD bytes must map to kSystemUnrecoverable"
        );

        free_wasmtime_context(context);
        free_wasmtime_global_context(global_context);

        let _: Box<i32> = Box::from_raw(core::mem::transmute(context_ptr));
    }
}

fn oversized_table_contract_bytes() -> &'static [u8] {
    static BYTES: std::sync::OnceLock<Vec<u8>> = std::sync::OnceLock::new();
    BYTES
        .get_or_init(|| {
            let wat = r#"
                (module
                  (table 65 funcref)
                  (func (export "0000000000000001"))
                  (memory 1 1)
                  (export "memory" (memory 0)))
            "#;
            let raw =
                wat::parse_str(wat).expect("oversized table WAT must parse");
            groundhog_wasm_meter::instrument(
                &raw,
                &groundhog_wasm_meter::Config::default(),
            )
            .expect("oversized table contract must be meter-valid")
        })
        .as_slice()
}

#[test]
fn test_h001_module_validate_skips_table_pool_limit() {
    let bytes = oversized_table_contract_bytes();
    let config =
        wasmtime_lib::global_context::WasmtimeGlobalContext::new_config();
    let engine = wasmtime::Engine::new(&config)
        .expect("production config should produce a valid host engine");

    let module_new_error = wasmtime::Module::new(&engine, bytes).expect_err(
        "Module::new must reject the table minimum against the pool limit",
    );
    let module_new_error = format!("{module_new_error:#}");
    assert!(
        module_new_error.contains("table")
            && module_new_error.contains("65")
            && module_new_error.contains("64"),
        "expected pooling allocator table-minimum rejection, got: {module_new_error}"
    );

    unsafe {
        let global_context = new_wasmtime_global_context(8);
        let context =
            new_wasmtime_context(no_get_mock_contract_get_fn, global_context);

        let verified =
            wasmtime_verify_contract(context, bytes.as_ptr(), bytes.len());
        assert_ne!(
            verified, 0,
            "upload verifier must reject contracts whose table minimum \
             exceeds the pooling allocator limit"
        );

        free_wasmtime_context(context);
        free_wasmtime_global_context(global_context);
    }
}

fn h004_read_var_u32(bytes: &[u8], pos: &mut usize) -> u32 {
    let mut result = 0u32;
    let mut shift = 0;
    loop {
        let byte = bytes[*pos];
        *pos += 1;
        result |= ((byte & 0x7f) as u32) << shift;
        if byte & 0x80 == 0 {
            return result;
        }
        shift += 7;
    }
}

fn h004_skip_name(bytes: &[u8], pos: &mut usize) {
    let len = h004_read_var_u32(bytes, pos) as usize;
    *pos += len;
}

fn h004_mutate_first_import_to_funcexact(mut wasm: Vec<u8>) -> Vec<u8> {
    let mut pos = 8;
    while pos < wasm.len() {
        let section_id = wasm[pos];
        pos += 1;
        let section_len = h004_read_var_u32(&wasm, &mut pos) as usize;
        let section_start = pos;
        let section_end = section_start + section_len;
        if section_id == 2 {
            let mut import_pos = section_start;
            let _import_count = h004_read_var_u32(&wasm, &mut import_pos);
            h004_skip_name(&wasm, &mut import_pos);
            h004_skip_name(&wasm, &mut import_pos);
            assert_eq!(wasm[import_pos], 0x00);
            wasm[import_pos] = 0x20;
            return wasm;
        }
        pos = section_end;
    }
    panic!("no import section found");
}

#[test]
fn test_h004_funcexact_import_rejected_by_verify() {
    let wat = r#"
        (module
          (type $syscall_t (func (param i32) (result i32)))
          (import "marmot" "syscall" (func $syscall (type $syscall_t)))
          (func (export "0000000000000001")
            i32.const 7
            call $syscall
            drop)
          (memory 1 1)
          (export "memory" (memory 0)))
    "#;
    let wasm = wat::parse_str(wat).expect("PoC WAT must parse");
    let instrumented = groundhog_wasm_meter::instrument(
        &wasm,
        &groundhog_wasm_meter::Config::default(),
    )
    .expect("PoC contract must instrument");
    let mutated = h004_mutate_first_import_to_funcexact(instrumented);

    unsafe {
        let global_context = new_wasmtime_global_context(8);
        let context =
            new_wasmtime_context(no_get_mock_contract_get_fn, global_context);

        let verified =
            wasmtime_verify_contract(context, mutated.as_ptr(), mutated.len());
        assert_ne!(
            verified, 0,
            "FuncExact import descriptor must be rejected at verify time"
        );

        free_wasmtime_context(context);
        free_wasmtime_global_context(global_context);
    }
}

fn append_u32_leb(mut value: u32, out: &mut Vec<u8>) {
    loop {
        let mut byte = (value & 0x7f) as u8;
        value >>= 7;
        if value != 0 {
            byte |= 0x80;
        }
        out.push(byte);
        if value == 0 {
            break;
        }
    }
}

fn append_custom_section(wasm: &[u8], section_name: &str) -> Vec<u8> {
    let mut out = wasm.to_vec();
    let mut custom_payload = Vec::new();
    append_u32_leb(section_name.len() as u32, &mut custom_payload);
    custom_payload.extend(section_name.as_bytes());
    out.push(0);
    append_u32_leb(custom_payload.len() as u32, &mut out);
    out.extend(custom_payload);
    out
}

#[test]
fn test_webidl_custom_section_rejected_by_verify() {
    let wasm_bytes = load_wasm_file("simple_add.wasm");
    let with_webidl = append_custom_section(wasm_bytes, "webidl-bindings");

    unsafe {
        let global_context = new_wasmtime_global_context(8);
        let context =
            new_wasmtime_context(no_get_mock_contract_get_fn, global_context);

        let verified = wasmtime_verify_contract(
            context,
            with_webidl.as_ptr(),
            with_webidl.len(),
        );
        assert_ne!(
            verified, 0,
            "webidl-bindings custom section must be rejected at verify time"
        );

        free_wasmtime_context(context);
        free_wasmtime_global_context(global_context);
    }
}

#[test]
fn test_wasm_interface_types_custom_section_rejected_by_verify() {
    let wasm_bytes = load_wasm_file("simple_add.wasm");
    let with_wit = append_custom_section(wasm_bytes, "wasm-interface-types");

    unsafe {
        let global_context = new_wasmtime_global_context(8);
        let context =
            new_wasmtime_context(no_get_mock_contract_get_fn, global_context);

        let verified = wasmtime_verify_contract(
            context,
            with_wit.as_ptr(),
            with_wit.len(),
        );
        assert_ne!(
            verified, 0,
            "wasm-interface-types custom section must be rejected at verify time"
        );

        free_wasmtime_context(context);
        free_wasmtime_global_context(global_context);
    }
}

fn float_contract_bytes() -> &'static [u8] {
    let wat = r#"
        (module
          (func (export "f") (result f32)
            (f32.const 0.0))
          (memory 1 1)
          (export "memory" (memory 0)))
    "#;
    let bytes = wat::parse_str(wat).expect("inline float WAT must parse");
    Box::leak(bytes.into_boxed_slice())
}

unsafe extern "C" fn test_get_float_contract_get_fn(
    _userctx: *mut c_void,
    hash: *const u8,
) -> ContractGetResult {
    let hash_slice = std::slice::from_raw_parts(hash, 32);
    if hash_slice.iter().any(|&b| b != 5) {
        return ContractGetResult::new(SystemErrorCode::Unrecoverable.0, &[]);
    }
    ContractGetResult::new(0, float_contract_bytes())
}

#[test]
fn test_h001_production_config_rejects_floats_at_validation() {
    let config =
        wasmtime_lib::global_context::WasmtimeGlobalContext::new_config();
    let engine = wasmtime::Engine::new(&config)
        .expect("production config should produce a valid host engine");
    let float_bytes = float_contract_bytes();

    let err = wasmtime::Module::new(&engine, float_bytes).err().expect(
        "production config must clear WasmFeatures::FLOATS so float-bearing \
         modules are rejected at validation on every host",
    );

    let msg = format!("{err:#}");
    assert!(
        msg.contains("floating-point"),
        "expected wasmparser validation rejection mentioning \
         \"floating-point\", got: {err:#}"
    );
}

#[test]
fn test_h001_verify_contract_rejects_float_module() {
    unsafe {
        let global_context = new_wasmtime_global_context(8);
        let context =
            new_wasmtime_context(no_get_mock_contract_get_fn, global_context);

        let wasm_bytes = float_contract_bytes();
        let result = wasmtime_verify_contract(
            context,
            wasm_bytes.as_ptr(),
            wasm_bytes.len(),
        );

        assert_ne!(
            result, 0,
            "float-bearing contracts must be rejected by \
             wasmtime_verify_contract on every host"
        );

        free_wasmtime_context(context);
        free_wasmtime_global_context(global_context);
    }
}

#[test]
fn test_validation_invalid_control_flow_rejected_deterministically() {
    let mut wasm = vec![0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00];

    // Type section: one no-arg, no-result function type.
    wasm.extend([0x01, 0x04, 0x01, 0x60, 0x00, 0x00]);
    // Function section: one function using type 0.
    wasm.extend([0x03, 0x02, 0x01, 0x00]);
    // Memory section: one memory, min 1 page, max 1 page.
    wasm.extend([0x05, 0x04, 0x01, 0x01, 0x01, 0x01]);
    // Global section: mutable i64 __groundhog_stack_limit, initialized to 0.
    wasm.extend([0x06, 0x06, 0x01, 0x7e, 0x01, 0x42, 0x00, 0x0b]);

    let stack_limit_export = b"__groundhog_stack_limit";
    let entrypoint_export = b"0000000000000001";

    let mut exports = Vec::new();
    exports.push(0x03); // 3 exports
    exports.push(0x06);
    exports.extend(b"memory");
    exports.extend([0x02, 0x00]); // memory export, index 0
    exports.push(stack_limit_export.len() as u8);
    exports.extend(stack_limit_export.as_slice());
    exports.extend([0x03, 0x00]); // global export, index 0
    exports.push(entrypoint_export.len() as u8);
    exports.extend(entrypoint_export.as_slice());
    exports.extend([0x00, 0x00]); // function export, index 0
    wasm.extend([0x07, exports.len() as u8]);
    wasm.extend(exports);

    let mut body = Vec::new();
    body.push(0x00); // local declaration count
    body.extend([
        0x23, 0x00, // global.get 0
        0x42, 0x3f, // i64.const 63
        0x54, // i64.lt_u
        0x04, 0x40, // if
        0x00, // unreachable
        0x0b, // end
        0x23, 0x00, // global.get 0
        0x42, 0x3f, // i64.const 63
        0x7d, // i64.sub
        0x24, 0x00, // global.set 0
        // Validation-invalid: block declares i32 result but body leaves i64
        0x41, 0x01, // i32.const 1
        0x04, 0x7f, // if (result i32)
        0x42, 0x00, // i64.const 0
        0x0b, // end
        0x23, 0x00, // global.get 0
        0x42, 0x3f, // i64.const 63
        0x7c, // i64.add
        0x24, 0x00, // global.set 0
        0x0b, // function end
    ]);

    let mut code = Vec::new();
    code.push(0x01); // 1 function body
    code.push(body.len() as u8);
    code.extend(body);
    wasm.extend([0x0a, code.len() as u8]);
    wasm.extend(code);

    let config =
        wasmtime_lib::global_context::WasmtimeGlobalContext::new_config();
    let engine = wasmtime::Engine::new(&config)
        .expect("production config should produce a valid host engine");
    wasmtime::Module::new(&engine, &wasm)
        .err()
        .expect("wasmtime must reject the validation-invalid control flow");

    unsafe {
        let global_context = new_wasmtime_global_context(8);
        let context =
            new_wasmtime_context(no_get_mock_contract_get_fn, global_context);

        let result =
            wasmtime_verify_contract(context, wasm.as_ptr(), wasm.len());

        assert_ne!(
            result, 0,
            "validation-invalid Wasm must be rejected deterministically, \
             not via debug abort"
        );

        free_wasmtime_context(context);
        free_wasmtime_global_context(global_context);
    }
}

#[test]
fn test_h001_runtime_creation_on_float_returns_system_unrecoverable() {
    unsafe {
        let global_context = new_wasmtime_global_context(8);
        let context = new_wasmtime_context(
            test_get_float_contract_get_fn,
            global_context,
        );
        let contract_hash = [5u8; 32];

        let context_ptr: *mut c_void =
            core::mem::transmute(Box::into_raw(Box::new(1)));

        let result =
            new_wasmtime_runtime(contract_hash.as_ptr(), context_ptr, context);

        assert!(
            result.get().is_none(),
            "float-bearing contract must not yield a runtime"
        );
        assert_eq!(
            result.err_code(),
            SystemErrorCode::Unrecoverable.0,
            "Module::new failure on float bytes must map to \
             kSystemUnrecoverable"
        );

        free_wasmtime_context(context);
        free_wasmtime_global_context(global_context);

        let _: Box<i32> = Box::from_raw(core::mem::transmute(context_ptr));
    }
}

// The production config must pin every wasm proposal it depends on, rather
// than inheriting a wasmtime default or relying on a side effect of the
// selected compiler strategy. Deployment admission is consensus-visible: if
// the accepted contract set can shift because an upstream default flipped or
// because someone swapped Strategy::Winch for Strategy::Cranelift, nodes can
// disagree on whether a contract is deployable. See issue #2535.

fn tail_call_module_bytes() -> Vec<u8> {
    let wat = r#"(module
        (memory (export "memory") 1)
        (func $tail)
        (func (export "call1") return_call $tail))"#;
    let raw = wat::parse_str(wat).expect("tail call wat should parse");
    groundhog_wasm_meter::instrument(
        &raw,
        &groundhog_wasm_meter::Config::default(),
    )
    .expect("wasm-meter allowlists tail calls, so instrumentation succeeds")
}

fn memory64_module_bytes() -> Vec<u8> {
    let wat = r#"(module
        (memory (export "memory") i64 1)
        (func (export "call1") (result i32) i64.const 0 i32.load))"#;
    wat::parse_str(wat).expect("memory64 wat should parse")
}

#[test]
fn test_production_config_rejects_tail_calls_under_any_strategy() {
    // wasm-meter allowlists ReturnCall/ReturnCallIndirect, so the engine is
    // the only gate. Before #2535 that gate was Strategy::Winch subtracting
    // WasmFeatures::TAIL_CALL in Config::validate rather than an explicit
    // policy, so selecting Cranelift silently made tail-call contracts
    // deployable. Pin the feature so the rejection survives a strategy change.
    let mut config =
        wasmtime_lib::global_context::WasmtimeGlobalContext::new_config();
    config.strategy(wasmtime::Strategy::Cranelift);
    config.signals_based_traps(false);
    config.table_lazy_init(true);

    let engine = wasmtime::Engine::new(&config)
        .expect("production config should produce a valid host engine");

    let err = wasmtime::Module::validate(&engine, &tail_call_module_bytes())
        .err()
        .expect(
            "production config must explicitly disable tail calls, so they \
             are rejected regardless of the compiler strategy",
        );

    assert!(
        format!("{err:#}").contains("tail calls"),
        "expected a tail-call validation rejection, got: {err:#}"
    );
}

#[test]
fn test_production_config_rejects_memory64_at_validation() {
    // wasmtime enables WasmFeatures::MEMORY64 by default and Winch does not
    // subtract it, so wasm-meter was the sole gate rejecting 64-bit memories.
    // The engine must reject them too: a 64-bit memory index type would change
    // contract-visible pointer width out from under the marmot::syscall ABI
    // (i32 pointers since #2422).
    let config =
        wasmtime_lib::global_context::WasmtimeGlobalContext::new_config();
    let engine = wasmtime::Engine::new(&config)
        .expect("production config should produce a valid host engine");

    let err = wasmtime::Module::validate(&engine, &memory64_module_bytes())
        .err()
        .expect(
            "production config must explicitly disable memory64 so 64-bit \
             memories are rejected at validation, not only by wasm-meter",
        );

    assert!(
        format!("{err:#}").contains("memory64"),
        "expected a memory64 validation rejection, got: {err:#}"
    );
}

fn extended_const_module_bytes() -> Vec<u8> {
    let wat = r#"(module
        (memory (export "memory") 1)
        (global i32 (i32.add (i32.const 1) (i32.const 2)))
        (func (export "call1")))"#;
    wat::parse_str(wat).expect("extended const wat should parse")
}

#[test]
fn test_production_config_rejects_extended_const_at_validation() {
    // The extended-const proposal adds i32/i64 add/sub/mul to the constant
    // expression grammar used by global initializers and data/element segment
    // offsets. It exists for dynamic linking (computing a segment offset as
    // __memory_base + N), which contracts have no use for. wasm-meter's
    // allowlist only classifies operators in function bodies, so constant
    // expressions never reach it — the engine is the only gate.
    let config =
        wasmtime_lib::global_context::WasmtimeGlobalContext::new_config();
    let engine = wasmtime::Engine::new(&config)
        .expect("production config should produce a valid host engine");

    wasmtime::Module::validate(&engine, &extended_const_module_bytes())
        .err()
        .expect(
            "production config must disable extended const so arithmetic in \
             constant expressions is rejected at validation",
        );
}

#[test]
fn test_production_config_accepted_proposal_surface_is_locked() {
    // The complete set of wasm proposals the production engine accepts. The
    // config denies every feature bit and then names what it re-enables, so
    // this table is the observable form of that decision: a proposal can only
    // move columns if someone edits new_config().
    //
    // This locks the surface against all three ways it has drifted or could
    // drift: an upstream default flipping on a wasmtime bump (memory64 was
    // enabled by default and gated only by wasm-meter), a compiler strategy
    // change (tail calls were rejected only because Strategy::Winch subtracts
    // TAIL_CALL), and an ordinary editing mistake in the config chain.
    let accepted: &[(&str, &str)] = &[
        ("reference_types", r#"(module (memory (export "memory") 1) (table 1 funcref) (func (export "call1") i32.const 0 ref.null func table.set 0))"#),
        ("bulk_memory", r#"(module (memory (export "memory") 1) (func (export "call1") i32.const 0 i32.const 0 i32.const 1 memory.copy))"#),
        ("multi_value", r#"(module (memory (export "memory") 1) (func (export "call1") (result i32) (block (result i32 i32) i32.const 1 i32.const 2) drop))"#),
        ("sign_extension", r#"(module (memory (export "memory") 1) (func (export "call1") (result i32) i32.const 1 i32.extend8_s))"#),
        ("call_indirect", r#"(module (memory (export "memory") 1) (table 1 funcref) (type $t (func)) (func (export "call1") i32.const 0 call_indirect (type $t)))"#),
        ("mutable_global", r#"(module (memory (export "memory") 1) (global $g (mut i32) (i32.const 0)) (func (export "call1") i32.const 1 global.set $g))"#),
    ];
    let rejected: &[(&str, &str)] = &[
        ("floats", r#"(module (memory (export "memory") 1) (func (export "call1") (result f32) f32.const 1))"#),
        ("simd", r#"(module (memory (export "memory") 1) (func (export "call1") v128.const i32x4 0 0 0 0 drop))"#),
        ("tail_call", r#"(module (memory (export "memory") 1) (func $t) (func (export "call1") return_call $t))"#),
        ("memory64", r#"(module (memory (export "memory") i64 1) (func (export "call1") (result i32) i64.const 0 i32.load))"#),
        ("extended_const", r#"(module (memory (export "memory") 1) (global i32 (i32.add (i32.const 1) (i32.const 2))) (func (export "call1")))"#),
        ("custom_page_sizes", r#"(module (memory (export "memory") 1 (pagesize 1)) (func (export "call1")))"#),
        ("multi_memory", r#"(module (memory (export "memory") 1) (memory 1) (func (export "call1")))"#),
        ("threads", r#"(module (memory (export "memory") 1) (func (export "call1") (result i32) i32.const 0 i32.atomic.load))"#),
        ("wide_arithmetic", r#"(module (memory (export "memory") 1) (func (export "call1") (result i64) i64.const 1 i64.const 1 i64.const 1 i64.const 1 i64.add128 drop))"#),
        ("gc", r#"(module (memory (export "memory") 1) (type $s (struct (field i32))) (func (export "call1") i32.const 1 struct.new $s drop))"#),
        ("function_references", r#"(module (memory (export "memory") 1) (type $s (func)) (func $t) (elem declare func $t) (func (export "call1") ref.func $t call_ref $s))"#),
        ("exceptions", r#"(module (memory (export "memory") 1) (tag $e) (func (export "call1") try_table (catch_all 0) end))"#),
    ];

    let config =
        wasmtime_lib::global_context::WasmtimeGlobalContext::new_config();
    let engine = wasmtime::Engine::new(&config)
        .expect("production config should produce a valid host engine");

    for (name, src) in accepted {
        let bytes = wat::parse_str(src)
            .unwrap_or_else(|e| panic!("{name}: wat should parse: {e}"));
        wasmtime::Module::validate(&engine, &bytes).unwrap_or_else(|e| {
            panic!(
                "{name} is part of the accepted contract set but the \
                 production config now rejects it: {e:#}"
            )
        });
    }

    for (name, src) in rejected {
        let bytes = wat::parse_str(src)
            .unwrap_or_else(|e| panic!("{name}: wat should parse: {e}"));
        assert!(
            wasmtime::Module::validate(&engine, &bytes).is_err(),
            "{name} is not part of the accepted contract set, but the \
             production config now admits it — deployment admission is \
             consensus-visible, so this must be a deliberate change"
        );
    }
}
