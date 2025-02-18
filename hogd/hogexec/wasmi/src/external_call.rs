#![allow(non_upper_case_globals)]
#![allow(non_camel_case_types)]
#![allow(dead_code)]
#![allow(non_snake_case)]

include!(concat!(env!("OUT_DIR"), "/bindings.rs"));

use wasmi::core::HostError;
use wasmi::Error;

use core::fmt;

/**
 * Error policy:
 * Syscalls should not be handled as "errors" returned from the syscall;
 * instead, syscalls return a uint64 that acts as the error code
 * (which the SDK can then check and call `unreachable`).
 *
 * There are two types of errors that we are concerned with here.
 * 1) System errors (which could be nondeterministic, i.e. malloc fail)
 * at which point we need to abort the whole transaction.
 * 2) OutOfGas errors, within the syscall.  We'd like to just return
 * an OutOfGas exception into wasmi, but wasmi does not expose
 * that as a public API, so we return a different error instead if
 * a syscall causes an OutOfGas error.
 * (The syscall will set the fuel to zero before returning in that case.
 * However, we do not have a guarantee that another instruction will execute
 * before the wasm function that called the syscall returns).
 * I'm using Wasmi's HostError API for this extra type of error.
 *
 */

#[repr(u8)]
#[derive(Clone, Copy, Debug)]
pub enum TrampolineErrorEnum {
    OutOfGas = 1,
    Unrecoverable = 2,
    Return = 3,
    Exit { code: i32 } = 4,
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
        x if x == kTrampolineErrorNONE => Ok(result.result),
        x if x == kTrampolineErrorOUTOFGAS => {
            Err(Error::host(TrampolineError {
                error: TrampolineErrorEnum::OutOfGas,
            }))
        }
        x if x == kTrampolineErrorRETURN => Err(Error::host(TrampolineError {
            error: TrampolineErrorEnum::Return,
        })),
        x if x == kTrampolineErrorEXIT => Err(Error::host(TrampolineError {
            error: TrampolineErrorEnum::Exit {
                code: result.result,
            },
        })),
        _ => Err(Error::host(TrampolineError {
            error: TrampolineErrorEnum::Unrecoverable,
        })),
    }
}
