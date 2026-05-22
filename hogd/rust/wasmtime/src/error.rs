#![allow(non_upper_case_globals)]
#![allow(non_camel_case_types)]
#![allow(dead_code)]
#![allow(non_snake_case)]

use core::fmt;
use core::fmt::Display;

use crate::invoke_result::WasmtimeInvokeResult;
use common::{to_invoke_result, HostFnErrorCode};

use std::error::Error;

#[derive(Debug, Copy, Clone)]
pub struct TrampolineError {
    pub error: HostFnErrorCode,
}

impl TrampolineError {
    pub fn to_wasmtime_invoke_result(
        self,
        gas_consumed: u64,
    ) -> WasmtimeInvokeResult {
        to_invoke_result(self.error).map_or_else(
            |err| WasmtimeInvokeResult::error(err, gas_consumed),
            |_| WasmtimeInvokeResult::success(gas_consumed),
        )
    }
}

impl Display for TrampolineError {
    fn fmt(&self, f: &mut fmt::Formatter) -> fmt::Result {
        write!(f, "TrampolineError: {:?}", self.error)
    }
}

impl Error for TrampolineError {}
