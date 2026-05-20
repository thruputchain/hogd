#![allow(non_upper_case_globals)]
#![allow(non_camel_case_types)]
#![allow(dead_code)]
#![allow(non_snake_case)]

use core::fmt;
use core::fmt::Display;
use wasmi::core::HostError;

use crate::wasmi_invoke_result::WasmiInvokeResult;
use common::{to_invoke_result, HostFnErrorCode};

#[derive(Debug, Copy, Clone)]
pub struct TrampolineError {
    pub error: HostFnErrorCode,
}

impl TrampolineError {
    pub fn to_wasmi_invoke_result(
        self,
        gas_consumed: u64,
    ) -> WasmiInvokeResult {
        to_invoke_result(self.error).map_or_else(
            |err| WasmiInvokeResult::error(err, gas_consumed),
            |_| WasmiInvokeResult::success(gas_consumed),
        )
    }
}

impl Display for TrampolineError {
    fn fmt(&self, f: &mut fmt::Formatter) -> fmt::Result {
        write!(f, "TrampolineError: {:?}", self.error)
    }
}

impl HostError for TrampolineError {}
