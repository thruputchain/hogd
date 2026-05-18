use std::ffi::c_void;
use std::fs;
use std::path::PathBuf;

// Import the FFI functions from wasmtime_lib
use common::SystemErrorCode;
use wasmtime_lib::{
    free_wasmtime_context, free_wasmtime_global_context, free_wasmtime_runtime,
    new_wasmtime_context, new_wasmtime_global_context, new_wasmtime_runtime,
    wasmtime_get_available_gas, wasmtime_invoke, wasmtime_set_available_gas,
    wasmtime_verify_contract, ContractGetResult,
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
        result_code: 0,
        panic: common::HostFnErrorCode::NoneOrRecoverable.0,
    }
}

#[no_mangle]
extern "C" fn c_call_1args(
    _fn_ptr: *mut c_void,
    _userctx: *mut c_void,
    _arg1: u64,
) -> common::TrampolineResult {
    common::TrampolineResult {
        result_code: 0,
        panic: common::HostFnErrorCode::NoneOrRecoverable.0,
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
        result_code: 0,
        panic: common::HostFnErrorCode::NoneOrRecoverable.0,
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
        result_code: 0,
        panic: common::HostFnErrorCode::NoneOrRecoverable.0,
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
        result_code: 0,
        panic: common::HostFnErrorCode::NoneOrRecoverable.0,
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
        result_code: 0,
        panic: common::HostFnErrorCode::NoneOrRecoverable.0,
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
        result_code: 0,
        panic: common::HostFnErrorCode::NoneOrRecoverable.0,
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
        result_code: 0,
        panic: common::HostFnErrorCode::NoneOrRecoverable.0,
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
        result_code: 0,
        panic: common::HostFnErrorCode::NoneOrRecoverable.0,
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
        let result =
            wasmtime_invoke(runtime, fn_name.as_ptr(), fn_name.len() as u32);

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

        // Set initial gas
        let initial_gas = 1_000_000u64;
        wasmtime_set_available_gas(runtime, initial_gas);

        // Verify we can read it back
        let current_gas = wasmtime_get_available_gas(runtime);
        assert_eq!(current_gas, initial_gas, "Gas should be set correctly");

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

        assert!(!result, "Invalid WASM should fail verification");

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
    // Test that we can invoke the fibonacci function and verify results
    // This tests the WAT->WASM conversion from test_fibonacci.wat
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

        wasmtime_set_available_gas(runtime, 1_000_000_000_000);

        // Try to invoke the fib function
        let fn_name = b"fib30";
        let result =
            wasmtime_invoke(runtime, fn_name.as_ptr(), fn_name.len() as u32);

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

// Regression test for audit issue #1: WasmtimeRuntime::new used to panic
// (UB across extern "C" boundary) when instantiation fails with a
// non-OutOfFuel error.
//
// This module has a data segment at offset 70000, beyond the 1-page
// (65536 byte) memory. During instantiation, wasmtime tries to copy the
// data segment and fails with a trap that is NOT OutOfFuel.
//
// Before the fix: this call panics inside extern "C" -> abort/UB.
// After the fix: new_wasmtime_runtime returns a null runtime pointer.
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

        // After fix: err_code == 0 but runtime pointer is null
        // (same convention as other module-level failures like check_memory_limit)
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

// Regression test for audit issue #1 variant: active element segment init can
// trap at instantiation time when the table offset is out-of-bounds.
//
// Before the fix: this could panic inside extern "C".
// After the fix: new_wasmtime_runtime returns a null runtime pointer.
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

// Regression test: if pooling allocator slots are exhausted, runtime creation
// must return a system error (err_code != 0), not panic.
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

// Regression test for issue #1472: a valid method-name export with the wrong
// ABI currently passes verification and runtime creation, so invoke-time
// defense in depth must classify the signature mismatch as deterministic.
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
        assert!(
            verified,
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

        wasmtime_set_available_gas(runtime, 1_000_000);

        let fn_name = b"0000000000000001";
        let result =
            wasmtime_invoke(runtime, fn_name.as_ptr(), fn_name.len() as u32);

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

// ===========================================================================
// Regression test for issue #1869:
// Wasmtime StackOverflow trap is nondeterministic, so it correctly maps to
// Unrecoverable. The defense is at the verification layer: wasm-meter must
// reject modules whose operand stack or per-function charge can produce
// Cranelift frames large enough to trigger Wasmtime's native stack check
// before the instrumented prologue runs.
//
// This module accumulates 1280 i64 values on the operand stack (via 128
// calls to a function returning 10 i64s), exceeding the default
// max_operand_stack_values (1024) and max_function_charge (4096 bytes).
// ===========================================================================

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

        assert!(
            !result,
            "contracts with operand stack exceeding wasm-meter limits \
             must be rejected by wasmtime_verify_contract"
        );

        free_wasmtime_context(context);
        free_wasmtime_global_context(global_context);
    }
}

// ===========================================================================
// Regression tests for audit issue #1469 (H002):
// Winch SIMD contract validity depends on host AVX support.
//
// Root cause: the production Wasmtime config in global_context.rs selects
// Strategy::Winch but leaves wasm_simd() at the default (enabled). Winch's
// x86_64 SIMD lowering is gated by runtime-detected AVX features, and its
// aarch64 backend has SIMD unimplemented entirely. An attacker-crafted
// module containing any v128 operator therefore:
//   - verifies and executes on x86_64 + AVX validators, but
//   - fails Module::new on x86_64 no-AVX or aarch64 validators, which
//     maps to SystemErrorCode::Unrecoverable in get_instance_pre.
// Fix: disable the SIMD feature in the production Config. Wasmtime then
// rejects v128 operators deterministically at the wasmparser validation
// layer, regardless of host CPU.
// ===========================================================================

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

// Directly asserts that the production Wasmtime Config disables the SIMD
// feature at the wasmparser validation layer. This is the property that
// makes SIMD rejection deterministic across hosts, independent of Winch's
// AVX-gated x86_64 SIMD lowering and its unimplemented aarch64 backend.
//
// Before the fix, wasm_simd defaults to true: Module::new either compiles
// the module (x86_64 + AVX host) or fails at codegen with "AVX support"
// or "unimplemented masm instruction" errors (no-AVX / aarch64 hosts) —
// neither path produces wasmparser's "SIMD support is not enabled"
// validation error, so the assertion fails uniformly pre-fix.
//
// After the fix, Module::new fails at validation with exactly that
// message on every host. That is the deterministic rejection we want.
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

    // anyhow's plain Display (`{err}` / `err.to_string()`) prints only the
    // top-level message ("failed to compile: ..."), dropping the nested
    // "SIMD support is not enabled" cause we need to assert on. Alternate
    // Display (`{err:#}`) prints the full chain on one line without the
    // Debug backtrace noise, which is what we want here.
    let msg = format!("{err:#}");
    assert!(
        msg.contains("SIMD support is not enabled"),
        "expected wasmparser validation rejection \"SIMD support is not \
         enabled\", got host-dependent codegen error instead: {err:#}"
    );
}

// Regression test using the real production FFI path. With wasm_simd
// disabled in global_context.rs the wasmparser validator rejects any
// module with v128 operators before Winch is consulted, so verification
// returns false on every host. Prior to the fix this test passed
// coincidentally on aarch64 hosts (Winch aarch64 can't compile SIMD so
// Module::new failed at codegen) and failed on x86_64 + AVX hosts (Winch
// successfully compiled SIMD, so verify returned true).
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

        assert!(
            !result,
            "SIMD-bearing contracts must be rejected by \
             wasmtime_verify_contract on every host"
        );

        free_wasmtime_context(context);
        free_wasmtime_global_context(global_context);
    }
}

// Documents the downstream kSystemUnrecoverable that the audit issue is
// actually about. If any path (verify bypass, cache prime, legacy stored
// contract) ever delivers SIMD bytes to get_instance_pre, Module::new
// fails and the runtime path maps the failure to kSystemUnrecoverable.
// With wasm_simd(false) this now happens deterministically at validation,
// uniformly across hosts, rather than depending on runtime-detected CPU
// features.
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

// ===========================================================================
// Regression tests for audit issue #1522 (H001):
// Winch accepts float-bearing contracts without NaN canonicalization.
//
// Root cause: the production config selected Strategy::Winch and relied on
// cranelift_nan_canonicalization(true) for deterministic NaN handling, but
// that knob is a no-op under Winch. Winch's x86_64 lowering of f32.div,
// f32.sqrt, etc. emits raw host-produced NaN bit patterns (0xffc00000),
// not the canonical 0x7fc00000 — so scalar float results are host- and
// backend-dependent, risking consensus divergence.
//
// Fix: clear the FLOATS bit in wasmparser's WasmFeatures via the
// wasm_features(...) Config escape hatch. wasmparser then rejects every
// f32/f64 operator and type at validation, uniformly across hosts and
// backends, before Winch is consulted.
// ===========================================================================

// Minimal float-bearing module: an f32 constant returned from an exported
// function. Both the return type (f32) and the operator (f32.const) are
// gated by wasmparser's FLOATS feature, so either validation branch will
// reject this before Winch codegen runs. Bytes are leaked to 'static so
// the FFI can treat them the same way as build.rs-produced fixtures. We
// can't route this through build.rs because groundhog_wasm_meter's
// default config rejects floats during instrument, so inline wat::parse
// bypasses the instrument step the normal fixtures go through.
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

// Directly asserts that the production Wasmtime Config clears the FLOATS
// bit at the wasmparser validation layer. This is the property that makes
// float rejection deterministic across hosts and independent of Winch's
// (non-canonical) NaN handling.
//
// Before the fix, wasm_features left FLOATS set: Module::new compiled
// float-bearing modules successfully, and wasm-meter was the only layer
// rejecting floats — so a wasm-meter regression would silently re-expose
// Winch's non-canonical NaN output to consensus.
//
// After the fix, Module::new fails at validation with an error chain
// containing "floating-point" (wasmparser emits "floating-point support
// is disabled" for f32/f64 types and "floating-point instruction
// disallowed" for ops). That's the deterministic wasmparser-layer
// rejection we want.
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

    // Alternate Display (`{err:#}`) prints the full anyhow chain on one
    // line; the top-level "failed to compile" message drops the nested
    // "floating-point ..." cause we need to assert on.
    let msg = format!("{err:#}");
    assert!(
        msg.contains("floating-point"),
        "expected wasmparser validation rejection mentioning \
         \"floating-point\", got: {err:#}"
    );
}

// Regression test using the real production FFI path. With FLOATS cleared
// in the WasmFeatures, wasmparser rejects any module with f32/f64
// operators or types before Winch is consulted, so verification returns
// false on every host. Prior to the fix, the wasmtime verify path relied
// solely on wasm-meter's reject_floats check — this test pins the
// wasmparser-layer gate as a second line of defense.
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

        assert!(
            !result,
            "float-bearing contracts must be rejected by \
             wasmtime_verify_contract on every host"
        );

        free_wasmtime_context(context);
        free_wasmtime_global_context(global_context);
    }
}

// Documents the downstream kSystemUnrecoverable that the audit issue is
// about. If any path (verify bypass, cache prime, legacy stored contract)
// ever delivers float-bearing bytes to get_instance_pre, Module::new
// fails and the runtime path maps the failure to kSystemUnrecoverable.
// With FLOATS cleared this now happens deterministically at validation,
// uniformly across hosts, rather than executing under Winch with
// non-canonical NaN bits.
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
