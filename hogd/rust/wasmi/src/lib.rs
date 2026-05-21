mod module_cache;
pub mod wasmi_context;
mod wasmi_error;
pub mod wasmi_invoke_result;
pub mod wasmi_runtime;

type SHA256Hash = [u8; 32];

pub use common::contract_loader::ContractGetFn;
pub use common::contract_loader::ContractGetResult;

#[cfg(feature = "debug_logging")]
#[macro_export]
macro_rules! wasmi_debug_log {
    ($e:expr) => {
        $e
    };
}

#[cfg(not(feature = "debug_logging"))]
#[macro_export]
macro_rules! wasmi_debug_log {
    ($e:expr) => {
        ()
    };
}
