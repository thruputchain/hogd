pub mod context;
pub mod error;
pub mod global_context;
pub mod instance_pre_cache;
pub mod invoke_result;
pub mod runtime;

type SHA256Hash = [u8; 32];

// Re-export FFI functions and types for testing and external use
pub use context::{
    free_wasmtime_context, new_wasmtime_context, wasmtime_verify_contract,
    ContractGetFn, ContractGetResult,
};
pub use global_context::{
    free_wasmtime_global_context, new_wasmtime_global_context,
    wasmtime_global_link_nargs,
};
pub use instance_pre_cache::InstancePreCache;

pub use invoke_result::WasmtimeInvokeResult;
pub use runtime::{
    free_wasmtime_runtime, new_wasmtime_runtime, wasmtime_get_module_stats,
    wasmtime_invoke, WasmtimeModuleStats,
};

#[cfg(feature = "debug_logging")]
#[macro_export]
macro_rules! debug_log {
    ($e:expr) => {
        $e
    };
}

#[cfg(not(feature = "debug_logging"))]
#[macro_export]
macro_rules! debug_log {
    ($e:expr) => {
        ()
    };
}
