#![allow(non_upper_case_globals)]
#![allow(non_camel_case_types)]
#![allow(dead_code)]
#![allow(non_snake_case)]

include!(concat!(env!("OUT_DIR"), "/bindings.rs"));

use wasmi::core::HostError;
use wasmi::Error;

use crate::ffi_enums::HostFnErrorCode;

use core::fmt;

#[repr(u8)]
#[derive(Clone, Copy, Debug)]
pub enum TrampolineErrorEnum {
    OutOfGas = 1,
    Unrecoverable = 2,
    Return = 3,
    Exit { code: i32 } = 4,
    ValidationFailure = 5,
}

#[derive(Debug, Copy, Clone)]
pub struct TrampolineError {
    pub error: TrampolineErrorEnum,
}

impl fmt::Display for TrampolineError {
    fn fmt(&self, f: &mut fmt::Formatter) -> fmt::Result {
        write!(f, "TrampolineError: {:?}", self.error)
    }
}

impl HostError for TrampolineError {}

// This is annoying, but I'm not sure how better to deal with this
pub fn handle_trampoline_error(result: TrampolineResult) -> Result<i32, Error> {
    match result.panic {
        x if x == HostFnErrorCode::kHostNoneOrRecoverable as i32 => {
            Ok(result.result)
        }
        x if x == HostFnErrorCode::kHostOutOfGas as i32 => {
            Err(Error::host(TrampolineError {
                error: TrampolineErrorEnum::OutOfGas,
            }))
        }
        x if x == HostFnErrorCode::kHostReturn as i32 => {
            Err(Error::host(TrampolineError {
                error: TrampolineErrorEnum::Return,
            }))
        }
        x if x == HostFnErrorCode::kHostExit as i32 => {
            Err(Error::host(TrampolineError {
                error: TrampolineErrorEnum::Exit {
                    code: result.result,
                },
            }))
        }
        x if x == HostFnErrorCode::kHostValidationFailure as i32 => {
            Err(Error::host(TrampolineError {
                error: TrampolineErrorEnum::ValidationFailure,
            }))
        }
        _ => Err(Error::host(TrampolineError {
            error: TrampolineErrorEnum::Unrecoverable,
        })),
    }
}
